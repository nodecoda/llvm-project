# 第 1 章：`tinytt` —— TTIR 是一个真 dialect

本章只做一件事：让 `tinytt` 成为一个**能被独立解析、验证、打印**的 MLIR
dialect，并由 `tiny-opt` 驱动。不做 lowering，不做布局，不做前端。

## 1. 为什么先做 dialect 而不是先做 kernel

前一条管线（GPUMin）的 TTIR 是私有 C++ IR（38 个 OpKind），它能跑通 kernel，
但它不是“阶段”：没有 parser、没有 verifier、没有 roundtrip，任何一级都可以
被绕过。tiny Triton 的第一个验收点就是把这个漏洞补上：

```text
如果一个 IR 不能被单独喂给工具，它就不是一个阶段。
```

## 2. 目录与构建注册

```text
tiny-triton/
  CMakeLists.txt                         # standalone 模板的 external 分支
  include/tiny/Dialect/TinyTT/*.td       # ODS：dialect / 类型 / 12 个 op
  lib/Dialect/TinyTT/IR/                 # verifier 与 dialect 注册
  tools/tiny-opt/                        # 驱动
```

这个目录不在 `mlir/` 树里，而是通过 external project 挂进当前 build：

```sh
tiny-triton/configure.sh /home/ubuntu/llvm/trees/pr-ctpop /home/ubuntu/llvm/builds/pr-ctpop
ninja -C /home/ubuntu/llvm/builds/pr-ctpop tiny-opt
```

`configure.sh` 只追加两个 cache 变量：

```text
LLVM_EXTERNAL_PROJECTS=tiny-triton
LLVM_EXTERNAL_TINY_TRITON_SOURCE_DIR=<repo>/tiny-triton
```

第一条让 `llvm/tools/CMakeLists.txt` 去遍历它；第二条给源目录。**只设第一条
不够**：`AddLLVM.cmake` 在源目录为空时会静默把 `LLVM_TOOL_TINY_TRITON_BUILD`
置为 OFF。

## 3. 类型：`!tinytt.ptr`

TTIR 里“一堆指针”必须是普通 tensor 值，否则 `addptr` 无法表达成 tensor op：

```mlir
!tinytt.ptr<f32>      // 全局内存，地址空间 1（默认，不打印）
!tinytt.ptr<f32, 2>   // shared/workgroup，第 3 章开始出现
```

`PtrType` 只有两个参数：pointee 与 addressSpace。地址空间是**类型的一部分**，
不是注释——这决定了 T5 里能不能把 `!tinytt.ptr<f32>` 直接映射成 `!llvm.ptr<1>`。

## 3.1 为什么 dialect 叫 `tinytt` 而不是 `tiny.tt`

MLIR 的扩展类型/op 前缀语法是 `!<dialect>.<mnemonic>` / `<dialect>.<op>`，解析器
在**第一个点**处切分。把 dialect 命名为 `tiny.tt` 会让 `!tiny.ptr<f32>` 被解析成
dialect `tiny`、类型 `ptr<f32>`，直接报 unregistered dialect。

所以本项目的命名是：

| 层 | dialect 名 | 类型写法 | op 写法 |
| --- | --- | --- | --- |
| TTIR | `tinytt` | `!tinytt.ptr<f32>` | `tinytt.addptr` |
| TTGIR（第 3 章） | `tinyttg` | `!tinytt.ptr<f32>` | `tinyttg.local_alloc` |

## 4. 十二个 op

| op | 语义 | 关键 verifier |
| --- | --- | --- |
| `tinytt.get_program_id` | 取 block 索引 | — |
| `tinytt.make_range` | 0..N-1 | 必须 1-D i32；本章 axis 只能是 0 |
| `tinytt.splat` | 标量广播成 tensor | 源不能是 tensor；元素类型必须一致 |
| `tinytt.addptr` | 指针张量 + 偏移 | 偏移必须是整型 tensor |
| `tinytt.load` | gather | ptr 必须是 `!tinytt.ptr` tensor；形状相等；pointee == 结果元素类型 |
| `tinytt.masked_load` | 带 mask 与 fallback 的 gather | 上述检查 + mask 是 i1 且同形 + other 元素类型一致 |
| `tinytt.store` | scatter | value 元素类型 == pointee；形状相等 |
| `tinytt.masked_store` | 带 mask 的 scatter | 上述检查 + mask 是 i1 且同形 |
| `tinytt.reduce` | 1-D 归约成标量 | combine ∈ {add,max,min}；输入必须 1-D；axis 必须是 0；结果不是 tensor |
| `tinytt.trans` | 维度置换 | order 必须是合法排列，且结果形状 = 置换后的输入形状 |
| `tinytt.broadcast` | 1 维扩展 | rank 不变；每个维度是 1 或已相等 |
| `tinytt.expand_dims` | 插入 1 维 | rank + 1；axis 合法；结果形状正确 |

verifier 不是装饰：第 4 条门禁就是“非法输入必须失败”。

## 5. 手写 golden：vecadd

`kernels/vecadd.ttir` 是第 2 章前端要对拍的基准：

```mlir
%idx = arith.addi %lanes, %base : tensor<1024xi32>
%pa  = tinytt.addptr %a, %idx
        : tensor<1024x!tinytt.ptr<f32>>, tensor<1024xi32> -> tensor<1024x!tinytt.ptr<f32>>
%va  = tinytt.load %pa : tensor<1024x!tinytt.ptr<f32>>, tensor<1024xf32>
```

注意这里**没有布局**。tensor 上不出现线程映射，这是 TTIR 与 TTGIR 的分界线。

## 6. 门禁

```sh
tiny-triton/scripts/check-t1.sh /home/ubuntu/llvm/builds/pr-ctpop/bin
```

五项：

1. `tiny-opt` 能运行；
2. 空 module roundtrip；
3. `kernels/vecadd.ttir` 能解析并重打印；
4. 两个负例被 verifier 拒绝（非指针 load、非法 combine）；
5. `tiny-opt --help` 里出现 `tinytt`。

## 7. 退出条件

- `ninja -C <build> tiny-opt` 成功，且 `build/bin/tiny-opt` 存在；
- `check-t1.sh` 5/5；
- `gpumin-verify-all.sh` 仍 8/8；
- `triton-verify-all.sh` 0 failed（本机未安装 `triton-opt`，T2–T4 依脚本设计 SKIP，T1/T5/T6/T7 必须 PASS）；
- `git tag tiny-t01` 指向本章 commit。
