# 第 51 章：严格 ABI——`ilp32` 下的硬件 F/D 调用边界

## 目标

第 40、41 章把 `ilp32f`/`ilp32d` 做通了，但一直有意拒绝“软浮点 ABI + 硬件
浮点指令”的组合。本章补上这条边界：

1. `-march=rv32if -mabi=ilp32` 与 `-march=rv32id -mabi=ilp32` 合法；
2. f32 以 **一个 GPR 的位模式**跨调用，f64 以 **偶数 GPR 对或 8 字节栈槽**
   跨调用；
3. 函数体内部仍用 `fadd.s`/`fadd.d`/`fmul.s`/`fmul.d`，不退回软浮点 libcall；
4. `ilp32f`、`ilp32d` 现有行为不回归；`ilp32f + D` 仍明确拒绝；
5. `ilp32` 下的调用参数/返回值有 lit 与真实运行两层证据。

完成之后，RVMin 对外口径是：

> **RV32IMAFD + C + Zicsr/Zifencei + Zve32x 子集**，
> 并且 `ilp32`/`ilp32f`/`ilp32d` 三种浮点 ABI 都能正确跨越调用边界。

## 背景：三种 ABI 与四种组合

RISC-V psABI 把“浮点参数怎么传”和“函数体用什么指令”分开了：

| ABI | f32 参数/返回 | f64 参数/返回 | 典型 `-march` |
| --- | --- | --- | --- |
| `ilp32` | 一个整数寄存器（位模式） | 偶数整数寄存器对或栈 | `rv32if` / `rv32id` |
| `ilp32f` | `fa0–fa7` | 仍走整数对（没有 D） | `rv32if` |
| `ilp32d` | `fa0–fa7` | `fa0–fa7` | `rv32id` |

前两种组合（`ilp32` + 硬件 F/D）常被误解为“软浮点”。它不是：**函数体仍然
执行硬件浮点指令**，只有调用边界把 FPR 值看成整数位模式。GCC/Clang 都允许
`-march=rv32id -mabi=ilp32`，这正是本章要实现的语义。

RV32D 有一个硬件限制：没有 `fmv.d.x`/`fmv.x.d`（那是 RV64 才有的指令）。
因此 f64 的位模式转换不能像 f32 那样一条 `fmv` 完成，必须经过内存：

    f64 参数：a0/a1 -> sw 到栈槽 -> fld 到 FPR
    f64 返回：FPR -> fsd 到栈槽 -> lw 到 a0/a1

这也解释了为什么本章的 f64 路径必须用私有栈槽，而不能照抄 f32 的 bitcast。

## 设计：一次委托 + 三处边界

### 1. CC 表委托

主调用约定表开头加一条运行时分派：

```tablegen
CCIf<"RVMinIsSoftFloatABI(State)", CCDelegateTo<CC_RVMin_SoftFloat>>,
```

`CC_RVMin_SoftFloat` 只做两件事：

```tablegen
CCIfType<[f32], CCBitConvertToType<i32>>,
CCIfType<[f64], CCCustom<"RVMinCC_AssignF64AsTwoI32">>,
```

返回值表同理（`RetCC_RVMin` → `RetCC_RVMin_SoftFloat`），f64 用
`RVMinCC_AssignF64Return` 固定 `X10/X11`。

### 2. 什么时候算“软 ABI”

`RVMinIsSoftFloatABI` 的判定（`RVMinISelLowering.cpp:51`）：

1. 模块标志里有 `target-abi=ilp32` → 软 ABI；
2. 没有显式标志时，`!hasStdExtF() && !hasStdExtD()` → 软 ABI；
3. 其余情况（`ilp32f`/`ilp32d`，或显式 `+f/+d` 推导）走原有硬浮点表。

这样 `llc -mattr=+f` 不带 `-target-abi` 时仍按硬浮点处理，只有 clang 显式
传 `-target-abi=ilp32`（或模块标志）时才触发软 ABI。行为可预测，也避免
误改第 40/41 章的默认路径。

### 3. f64 的寄存器对分配

`RVMinCC_AssignF64AsTwoI32` 只接受 **偶数起始** 的整数对：

- `X10/X11`、`X12/X13`、`X14/X15`、`X16/X17`；
- 每一半各 `getCustomReg(..., MVT::i32, BCvt)`；
- 找不到空对时 `AllocateStack(8, 8)`，再用 `getCustomMem` 给出两个 4 字节位置。

这条规则直接对应 psABI 的“even-aligned register pair”，也是后面
`f64_many`（第 5 个 double 走栈）能正确工作的原因。

### 4. 三处 lowering

- `LowerFormalArguments`：先按 `VA.getValNo()` 把 `ArgLocs` 聚合成 `Pieces`，
  两个 i32 + f64 时调用 `joinI32PairToF64`；
- `LowerCall`：参数循环遍历 `ArgLocs`，`LocInfo == BCvt` 时 f32 走 `BITCAST`，
  f64 只拆一次，按 `NextHalf` 取低/高半；
- `LowerReturn`：同样的拆分逻辑 + `CopyToReg`；
- `splitF64ToI32Pair` / `joinI32PairToF64` 都使用 8 字节私有栈槽，并用
  `DAG.getEntryNode()` 接链，避免第 48 章遇到的 cycle。

## 代码地图

| 位置 | 职责 |
| --- | --- |
| `RVMinCallingConv.td:28-49` | 软 ABI 的 CC 表与主表委托 |
| `RVMinCallingConv.td:69` | 返回值的软 ABI 委托 |
| `RVMinISelLowering.cpp:51` | `RVMinIsSoftFloatABI` 判定 |
| `RVMinISelLowering.cpp:69` | f64 参数：偶数对或 8 字节栈 |
| `RVMinISelLowering.cpp:100` | f64 返回：固定 `X10/X11` |
| `RVMinISelLowering.cpp:945` | `splitF64ToI32Pair` |
| `RVMinISelLowering.cpp:965` | `joinI32PairToF64` |
| `RVMinISelLowering.cpp:1049-1087` | 形参按 `ValNo` 聚合 |
| `RVMinISelLowering.cpp:1204` | 调用参数拆分 |
| `RVMinISelLowering.cpp:1301` | 调用返回值拼回 |
| `RVMinISelLowering.cpp:1374` | 返回拆分 |
| `RVMinISelLowering.cpp:413-424` | 删除 `ilp32 + F` 的误拒绝 |
| `clang/lib/Basic/Targets/RVMin.cpp:119-125` | 删除 clang 驱动的 `ilp32 + F` 误拒绝 |
| `scripts/rt/abi-soft.c` | 第 51 章专用 ABI 冒烟程序 |
| `scripts/rvmin-run.sh:384-430` | 第 51 章运行门禁 |
| `scripts/rvmin-clang-run.sh` | `--target=rvmin` 一体化的 `ilp32 + F/D` 运行门禁 |
| `clang/test/Driver/rvmin-target.c` | 驱动接受 `rv32if/rv32id + ilp32` |

## 关键实现

### f32 的两条方向

```asm
addf:                       # float addf(float a, float b), ilp32
  fmv.w.x fa0, a1           # b: a1 -> fa0
  fmv.w.x fa1, a0           # a: a0 -> fa1
  fadd.s  fa0, fa1, fa0
  fmv.x.w a0, fa0           # 返回：fa0 -> a0
  jalr    zero, 0(ra)
```

### f64 的两半

```asm
addd:                       # double addd(double a, double b), ilp32
  addi sp, sp, -32
  sw   a3, 20(sp)
  sw   a2, 16(sp)
  sw   a1, 28(sp)
  sw   a0, 24(sp)
  fld  fa0, 16(sp)          # b 的低/高半拼回
  fld  fa1, 24(sp)          # a 的低/高半拼回
  fadd.d fa0, fa1, fa0
  fsd  fa0, 8(sp)
  lw   a0, 8(sp)            # 返回低半
  lw   a1, 12(sp)           # 返回高半
  addi sp, sp, 32
  jalr zero, 0(ra)
```

第 5 个 double 走栈时，`f64_many` 能看到调用方的 `fsd` 与 `fld fa0, 80(sp)`；
这说明 8 字节栈槽与偶数对规则在同一套 CC 下工作。

## 验证

### 1. lit：`llvm/test/CodeGen/RVMin/abi-soft-float.ll`

| 段 | 配置 | 断言 |
| --- | --- | --- |
| `SOFT` | `+f,+d -target-abi=ilp32` | f32/f64 参数与返回走 GPR/栈，算术保留 `fadd.s/fadd.d` |
| `SOFTF` | `+f -target-abi=ilp32` | 只有 F 时 f64 仍按整数对走 |
| 重汇编 | `llvm-mc -triple=riscv32 -mattr=+f,+d` | `.text` 逐字节一致 |
| `BADF` | `+f,+d -target-abi=ilp32f` | 仍 `not llc`，诊断清晰 |

### 2. 运行：为什么不能只看 `float.c`

第 42 章的 `float.c` 在 `-O2` 下并不覆盖“f32 返回值跨调用”：

- `add9f(1,2,...,9)` 被 clang 常量折叠，汇编里没有该函数，也没有调用；
- `i2f()` 返回 volatile 全局 `fsink`，实际生成的是 `lw a0, 0(a1)`；
- 剩下唯一的 FPR→GPR 路径是 `f2i()` 的 `fcvt.w.s`。

因此本章新增 `scripts/rt/abi-soft.c`：每个 helper 都吃 volatile 输入，并且
带一点算术（`x * 2.0f` / `x * 2.0`）阻止 LLVM 用 `returned` 属性把调用
替换成实参。实际汇编：

```asm
fpass:
  fmv.w.x fa0, a0
  fadd.s  fa0, fa0, fa0
  fmv.x.w a0, fa0        # FPR -> GPR，位模式返回
  jalr    zero, 0(ra)

dpass:
  sw   a1, 12(sp)
  sw   a0, 8(sp)         # a0/a1 -> 栈槽
  fld  fa0, 8(sp)
  fadd.d fa0, fa0, fa0
  fsd  fa0, 0(sp)
  lw   a0, 0(sp)         # 返回低半
  lw   a1, 4(sp)         # 返回高半
  jalr zero, 0(ra)
```

程序在 QEMU 上输出 `rvmin: ok`，退出码 0；`readelf -r` 中没有
`__addsf3/__adddf3/__fixsfsi/__floatsisf`。

### 3. 门禁

| 项 | 结果 |
| --- | --- |
| `llvm-lit -s CodeGen/RVMin MC/RVMin` | **36/36 PASS** |
| `llvm-lit` 的 4 个 rvmin clang 测试 | **4/4 PASS** |
| `rvmin-run.sh` | 第 51 章两段 PASS（`f32-soft` + `abi-soft`） |
| `rvmin-clang-run.sh` | `ilp32 + hardware F/D ran through the clang driver` |
| `rvmin-verify-all.sh` | **8/8** |
| llc | **216** 次（24 个 `.ll` − 6 个 `RUN: not llc` = 18 × 4 O × 3 mattr） |
| clang 矩阵 | **432** 对象（9 ISA × 4 O × 12 程序） |
| e2e | **288** 对象（`scripts/e2e/*.c` 未变） |
| DSL 契约 | `dsl-surface.py --check` 全 OK |

## 计数与口径

| 项 | 数值 | 说明 |
| --- | --- | --- |
| 编码语料 | 191 + 27 V | 第 47–50 章既有；本章不加指令 |
| 反汇编 V | 27 条 | `.text` 逐字节往返 |
| llc | **216** | 新增 `abi-soft-float.ll` 同时带 `RUN: not llc`，抵消文件增量 |
| clang 矩阵 | **432** | `scripts/e2e/` 仍 12 个程序 |
| e2e | **288** | `scripts/rt/abi-soft.c` 不在矩阵 glob 内 |
| lit | **36** | CodeGen 24 + MC 12 |

**不要把计数改成 228**：24 个文件里有 6 个 `RUN: not llc`
（`abi-soft-float`、`d-ext`、`f-ext`、`tail-call-musttail-error`、
`v-ext-abi-arg`、`v-ext-abi-ret`），可编译文件仍是 18 个。

## 常见坑

1. **不要拿 `float.c` 断言 `fmv.x.w`。** clang 会折叠 `add9f` 调用，`i2f`
   又从内存返回；用 `abi-soft.c` 的 volatile 输入才稳定。
2. **不要用 `returned` 语义的 pass-through helper。** LLVM 会把
   `float f(float x){return x;}` 的调用直接换成实参，ABI 转换不会生成；
   让 helper 做一点算术最省事。
3. **f64 必须走偶数对。** `X10/X11`、`X12/X13`…… 不能从 `a1/a2` 开始；
   否则与 GCC/psABI 不一致。
4. **8 字节栈槽要 `DAG.getEntryNode()` 接链。** 否则 SelectionDAG 可能把
   store/load 折叠成环，第 48 章的 cycle 问题会复现。
5. **`ilp32f + D` 仍然拒绝。** 它会让 f64 静默走 `fa0–fa7`（那是 `ilp32d`），
   本章没有放开这个组合。
6. **varargs 仍不支持。** 软 ABI 下可变参数还要处理 `al` 寄存器与整数/浮点
   分类，留给后续章节。
7. **模块标志优先。** `-target-abi=ilp32` 会写进 IR 的 `"target-abi"` 模块
   标志；只看 `-mattr` 推导会误判。
8. **只改后端不够。** `clang/lib/Basic/Targets/RVMin.cpp` 里还有一份
   ABI/扩展检查；第 51 章删掉了 `ilp32 + F` 的拒绝，否则
   `clang --target=rvmin -march=rv32if -mabi=ilp32` 仍然会报错。

## 练习

1. 给 `abi-soft.c` 加第 9 个 float/double 参数，验证栈上的两半顺序。
2. 实现软 ABI 下的 `printf` 风格 varargs：先做 `va_start` 的 GPR 保存区。
3. 把 `ilp32f + D` 从“拒绝”改成“按 `ilp32d` 处理”，并写清楚为什么
   这等价于让用户显式选择 `ilp32d`。
4. 用 `riscv32-unknown-elf-gcc -march=rv32id -mabi=ilp32` 编译同一
   `abi-soft.c`，对比调用点寄存器分配。
