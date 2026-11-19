# 第 5 章：TTGIR → LLVM dialect —— 标量化与 kernel ABI

前四章的 IR 都是“块级”的：一个 `tensor<1024xf32>` 是一个值。GPUMin 的
机器码里没有这种东西——每个线程只有寄存器里的标量。这一章完成从块级
程序到线程级程序的跳跃：

```sh
tiny-opt vecadd.ttgir --tiny-convert-ttg-to-llvm -o vecadd.llir
mlir-translate --mlir-to-llvmir vecadd.llir -o vecadd.ll
llc -march=gpumin -O1 vecadd.ll -o vecadd.s
```

这是整条管线里风险最高的一步：值会被复制、指针要按线程展开、调用约定
必须与后端完全一致。本章只做 vecadd 需要的那一组 op。

## 1. 为什么是一个 pass，而不是两个

规划里曾把这一步拆成 `--tiny-scalarize` 与 `--tiny-convert-ttg-to-llvm`。
实现时发现前者无法独立存在：标量化之后的值是“每个线程一个标量”，这个
中间形态既不是 `tinyttg`（它的 op 要求 tensor），也不是 `llvm`（还没
降到目标 op）。没有一个已注册 dialect 能承载它，单独暴露一个 pass 只会
制造一个无法 parse 的中间态。

因此本章是一个 pass，内部维护 lane vector：

```cpp
struct LaneValue { SmallVector<Value> elts; };   // 标量 1 个，张量 N/32 个
```

`DenseMap<Value, LaneValue>` 把每个 TTIR 值映射到它在线程里的标量表示。
这是 pass 的私有数据结构，不是 IR。

## 2. lane vector 的编号

第 3 章说过布局是 tile 重复。对 1D、`sizePerThread=[1]` 的默认布局：

```text
tensor 元素 r*32 + tid  ->  线程 tid 的第 r 个寄存器
```

所以 `tensor<1024xi32>` 变成 32 个 `i32` 值：`make_range` 为每个 r 生成
`r*32 + tid`。`splat` 把标量复制 32 份（同一 SSA 值重复使用）。

`elementsPerThread()` 不写死 32，而是从布局算：

```text
count = product(shape[d] / tile[d]) * product(sizePerThread[d])
```

第 7 章加 2D 布局时这个函数不用改。

## 3. kernel ABI

与 GPUMin 教程第 24 章完全一致：

```llvm
define void @vecadd() #0 {
  %1 = call i32 @llvm.gpumin.tid()
  %2 = call i32 @llvm.gpumin.args()
  ...
}
attributes #0 = { "gpumin_kernel" }
```

三点必须对齐：

1. **没有形参**。launcher 把参数块地址写进 `sr.args`，kernel 用
   `llvm.gpumin.args` 读出来；
2. 参数块是 32 位字数组：第 i 个参数在 `base + 4*i`，`load i32` 后指针参数
   再 `inttoptr` 到 `!llvm.ptr<1>`；
3. `gpumin_kernel` 必须作为函数属性出现在 LLVM IR 里，后端靠它区分
   kernel 与普通函数。MLIR 侧通过
   `passthrough = ["gpumin_kernel"]` 导出：
   `attributes #0 = { "gpumin_kernel" }`。

module 上写 `llvm.target_triple = "gpumin-unknown-elf"`，`mlir-translate`
会把它变成 `target triple` 行。

## 4. 指令映射（本章范围）

| TTGIR | LLVM dialect |
| --- | --- |
| `tinyttg.get_program_id <axis=0>` | `llvm.call_intrinsic "llvm.gpumin.blockidx"` |
| `tinyttg.make_range` | `mul` + `add` + `llvm.gpumin.tid` |
| `tinyttg.splat` | 同一个标量重复使用 |
| `tinyttg.addptr` | `ptrtoint` → `add`（offset×4）→ `inttoptr` |
| `tinyttg.load` / `store` | 每元素 `llvm.load` / `llvm.store` |
| `arith.addi/muli/...` | `llvm.add/mul/...`（逐 lane） |
| `arith.addf/mulf/...` | `llvm.fadd/fmul/...`（逐 lane） |
| `func.return` | `llvm.return` |

`arith` 只处理两个操作数、一个结果的标量/逐元素形式。这是 TTIR 里唯一
会出现的形状；超出就报错，不猜。

## 5. 为什么指针用 inttoptr 而不是 GEP

opaque pointer 下 `llvm.getelementptr` 仍要求显式 `elementtype`，而 GPUMin
的地址模型是 32 位平坦地址。本章直接用：

```text
addr = ptrtoint base
addr = addr + offset * 4
p    = inttoptr addr
```

它与旧 `LLVMEmit` 的写法一致，也避开 GEP 在 opaque pointer 语法上的版本
差异。等第 7 章需要按行/列跨步时，再把步长换成布局推导出来的值即可。

## 6. 端到端验证

`check-t5.sh` 的第 5 项不是“跑通就算”，而是证明**寻址正确**：

- `a[0..512)` 填 `1.0f`，`a[512..1024)` 填 `2.0f`，`b` 全 0；
- dump `c[0..1024)`；
- 断言前 512 个字是 `1.0f` 的位模式、后 512 个是 `2.0f`。

如果线程映射把元素写到错误地址，或只覆盖了第一个 tile，这个断言会失败；
只比较前 8 个值则不会。

> 规划里曾担心 1024 元素会触发旧后端的 predicate spill。本章的 lowering
> 实测可以编译并跑完 1024 元素（32 元素/线程），因此 e2e 直接用真实的
> `kernels/vecadd.py` 尺寸，不需要 256 的替代 kernel。

## 7. 已知边界（第 6、7 章补齐）

- `masked_load/masked_store`：需要谓词化与 select 的 CFG；
- `masked_load/masked_store` 之外的 op 都已补齐：1-D reduce（第 6 章）、
  `expand_dims/broadcast`（第 7a）、`trans`、`convert_layout`、2-D reduce、
  `exp2`、`local_*`、`barrier`（第 7b）；
- 2D 布局：`kernels/copy2d.py`（7a）与 `transpose2d/softmax2d/matmul2d`（7b）
  都是端到端用例；
- `masked_load/masked_store` 仍需要谓词化与 select 的 CFG。

遇到这些 op，pass 报
`no LLVM lowering for this operation in chapter 5` 并失败，不静默跳过。

## 8. 门禁

```sh
tiny-triton/scripts/check-t5.sh /home/ubuntu/llvm/builds/pr-ctpop/bin
```

五项：

1. pass 能跑通 `tests/passes/vecadd.ttgir`；
2. 产物里没有 `tensor<`、没有 `tinyttg.*` op，且含 `llvm.func @vecadd` 与
   `llvm.gpumin.args`；
3. 产物与 `tests/passes/vecadd.llir` 逐字节一致；
4. `mlir-translate` + `llc -march=gpumin -verify-machineinstrs` 成功，汇编含
   `sr.args/sr.tid/sr.blockidx` 与 `exit`；
5. 模拟器输出 1024 个字，前半 `1.0f`、后半 `2.0f`。

## 9. 退出条件

- `ninja -C <build> tiny-opt` 成功；
- `check-t5.sh` 5/5；`check-t1.sh`–`check-t4.sh` 全绿；
- 旧管线 `gpumin-verify-all.sh` 8/8、`triton-verify-all.sh` 0 failed；
- `git tag tiny-t05` 指向本章 commit。
