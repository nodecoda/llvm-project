# 第 8 章：阶段续跑与全量门禁

前七章把管线拆成了三个 checkpoint：

```text
kernel.py ──frontend──▶ TTIR ──tt-to-ttg──▶ TTGIR ──ttg-to-llvm──▶ LLIR
                         │                   │                    │
                    tinytt dialect      tinyttg dialect      llvm dialect
```

每一级都是完整、可独立 `parse`/`verify`/`print` 的 MLIR module。
本章只做一件事：**让驱动在这三个 checkpoint 上可进可出，并且禁止跳站。**

为什么要“禁止跳站”？因为跳站会绕过被跳过的 IR 上的 verifier。
`TTIR → LLIR` 一次跑完，中间布局分配错误就只能靠最后的汇编或模拟器
结果暴露；而 `TTIR → TTGIR` 之后先 `--verify-each` 一遍，错误会在
布局契约那一层立刻报出来。教程里每一章对应一个 checkpoint，
checkpoint 就是可复查的证据。

## 1. CLI 契约

```text
tiny-triton <kernel.py>              --emit ttir|ttgir|llir
tiny-triton --input ttir  k.ttir     --emit ttgir
tiny-triton --input ttgir k.ttgir    --emit llir
```

| 输入 | `--input` | `--emit` | 结果 |
| --- | --- | --- | --- |
| `kernel.py` | 省略 | `ttir` | 前端产出 TTIR |
| `kernel.py` | 省略 | `ttgir` | 前端 → TTGIR |
| `kernel.py` | 省略 | `llir` | 前端 → TTGIR → LLIR |
| `k.ttir` | `ttir` | `ttir` | 原样 roundtrip |
| `k.ttir` | `ttir` | `ttgir` | 跑 `tiny-convert-tt-to-ttg` |
| `k.ttgir` | `ttgir` | `ttgir` | 原样 roundtrip |
| `k.ttgir` | `ttgir` | `llir` | 跑 `tiny-convert-ttg-to-llvm` |
| `k.ttir` | `ttir` | `llir` | **拒绝**：`run --emit ttgir first` |
| `k.ttgir` | `ttgir` | `ttir` | **拒绝**：`cannot go back from ttgir to ttir` |

`--input` 缺省为空，表示输入是 Python；`--emit` 缺省为 `ttir`，
所以第 2–7 章的命令不受影响。`--input llir` 不是合法输入：LLIR 是
终态，不能从它继续往“更下游”跑。

## 2. 实现：一个 enum，三个入口

`tools/frontend/main.cpp` 里只有三样东西：

```cpp
enum class Stage { TTIR, TTGIR, LLIR };
std::optional<Stage> parseStage(StringRef);   // "ttir" / "ttgir" / "llir"
LogicalResult runPass(ModuleOp, Stage to);    // 跑一个已注册的 pass
```

`runPass` 用的是 `tiny-opt` 注册的**同一批** pass 对象：

```cpp
PassManager manager(module.getContext());
manager.enableVerifier(true);
if (to == Stage::TTGIR)
  manager.addPass(tiny::createTinyTTToTinyTTGPass());
else
  manager.addPass(tiny::createTinyTTGToLLVMPass());
return manager.run(module);
```

因为 pass 对象、顺序和 verifier 开关都一致，`tiny-triton --input ttir
k.ttir --emit ttgir` 与 `tiny-opt --tiny-convert-tt-to-ttg k.ttir`
的输出**逐字节相同**（本章门禁就是 `cmp`）。打印时在 module 之后补一个
换行，与 `mlir-opt` 系列的输出习惯对齐。

三条入口共用同一段尾逻辑：

```text
Python 输入： AST → emitTTIR → (需要时) TTIR→TTGIR → (需要时) TTGIR→LLIR
.mlir 输入： parse → (需要时) 跑一个 pass → 打印
```

中间不经过任何字符串拼接：上一阶段的 `ModuleOp` 直接交给下一阶段的
`PassManager`。

## 3. 三个可复现实验

```sh
BIN=/home/ubuntu/llvm/builds/pr-ctpop/bin

# 从 TTIR checkpoint 续跑到 TTGIR
$BIN/tiny-triton --input ttir kernels/vecadd.ttir --emit ttgir -o /tmp/a.ttgir
$BIN/tiny-opt --tiny-convert-tt-to-ttg kernels/vecadd.ttir -o /tmp/b.ttgir
cmp /tmp/a.ttgir /tmp/b.ttgir && echo same

# 从 TTGIR checkpoint 续跑到 LLIR
$BIN/tiny-triton --input ttgir /tmp/a.ttgir --emit llir -o /tmp/a.llir
$BIN/tiny-opt --tiny-convert-ttg-to-llvm /tmp/a.ttgir -o /tmp/b.llir
cmp /tmp/a.llir /tmp/b.llir && echo same

# Python 直接到 LLIR：内部仍然依次经过两个 checkpoint
$BIN/tiny-triton kernels/vecadd.py --emit llir \
  --arg-type a_ptr=ptr --arg-type b_ptr=ptr --arg-type c_ptr=ptr -o /tmp/py.llir
cmp /tmp/py.llir /tmp/a.llir && echo same
```

第三条能 `cmp` 成功，说明“一次调用跑完三个阶段”和“三次调用分别跑”
在语义上完全一致——阶段是可以停下来检查的，不是内部实现细节。

## 4. 被拒绝的两种走法

```sh
$BIN/tiny-triton --input ttir kernels/vecadd.ttir --emit llir
# tiny-triton: cannot lower TTIR directly to LLIR; run --emit ttgir first
# 退出码 2

$BIN/tiny-triton --input ttgir kernels/vecadd.ttgir --emit ttir
# tiny-triton: cannot go back from ttgir to ttir
# 退出码 2
```

诊断文本是门禁的一部分：它必须指出**下一步该跑哪个阶段**，而不是
只说“非法”。`--input` 只接受 `ttir|ttgir`，`--input llir` 直接拒绝。

## 5. 门禁

```sh
tiny-triton/scripts/check-t8.sh /home/ubuntu/llvm/builds/pr-ctpop/bin
```

五项：

1. `--input ttir kernels/vecadd.ttir --emit ttgir` 与 `tiny-opt` 输出、
   以及 `tests/passes/vecadd.ttgir` golden 都逐字节相同；
2. `--input ttgir --emit llir` 同样与 `tiny-opt` 和 T5 golden 相同；
3. `TTIR → LLIR` 与 `TTGIR → TTIR` 都返回非 0，且诊断文本正确；
4. `kernels/vecadd.py --emit ttir|ttgir|llir` 三者分别与续跑路径相同，
   且 TTGIR/LLIR 都能被 `tiny-opt --verify-each` 接受；
5. `--help` 公开 `--input`/`--emit` 与三个阶段名，`docs/08` 记录同一份契约。

## 6. 全量门禁

```sh
tiny-triton/scripts/verify-all.sh /home/ubuntu/llvm/builds/pr-ctpop/bin
```

脚本按顺序跑 `check-t1.sh` … `check-t8.sh`，再跑
`gpumin-verify-all.sh` 与 `triton-verify-all.sh`；任何一项失败立即退出。
第 7 章拆成 7a/7b 两个脚本，`check-t7.sh` 负责按顺序调用两者，
`verify-all.sh` 只认章号。

这是本课程唯一的“全绿”定义。提交前跑一次，输出应包含：

```text
chapter 8: 5 passed, 0 failed
gpumin-verify-all: 8 passed, 0 failed
triton-verify-all: 4 passed, 0 failed, 3 skipped
verify-all: all tiny-triton chapters and GPUMin gates are green
```

## 7. 已知边界

- `runPass` 只接受“相邻一站”，没有 `--emit` 之外的目标；
- 没有 `--input llir`（LLIR 不能反向重建 TTGIR）；
- 不做文本级 `sed` 式改写：所有阶段转移都是 MLIR pass；
- 驱动程序不隐藏中间结果：需要时把 `-o` 指向文件，逐站 `--verify-each`。

## 8. 退出条件

- `ninja -C <build> tiny-opt tiny-triton` 成功；
- `check-t8.sh` 5/5；
- `scripts/verify-all.sh <bin>` 全绿；
- `docs/02` 不再声称“只支持 `--emit ttir`”，`docs/01–08` 的阶段图与 CLI 表
  与本实现一致；
- `git tag tiny-t08` 指向本章 commit。
