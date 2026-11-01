# 现状：GlobalISel legalizer C++ 是怎么被测试的

版本：`sync/table-classify`，2026-11-02。方法：只检查本 worktree 的
`llvm/unittests`、`llvm/test`、`llvm/lib/Target/*/GISel` 和
`llvm/utils/TableGen`；计数来自 `find` / `grep`。

## 0. 结论摘要

现有 GlobalISel legalizer 测试可以分成五层：

| 层 | 位置 | 规模 | 主要 oracle |
|---|---|---|---|
| 规则构建器 / 查询语义 | `LegalizerInfoTest.cpp` | 4 个 TEST | 直接调 `LegalizerInfo::getAction()` |
| 单条合法化变换 | `LegalizerHelperTest.cpp` | 102 个 TEST_F | 期望 MIR |
| 整条 legalization | `LegalizerTest.cpp` + 各目标 MIR lit | 3 个 TEST_F；AArch64 173、AMDGPU 191、X86 72 个 lit | 期望 MIR / `FailedOn` |
| 规则集结构验证 | `LegalizerInfo::verify()` + `legalizer-info-validation.mir` | 仅 AArch64、RISCV 有 lit | 覆盖度检查、`ill-defined` 断言 |
| 生成器 / 规则 DSL | `llvm/test/TableGen/legalizer-info*.td` | 30 个 `.td` | 生成 C++/JSON/table `.inc` 的 FileCheck |

表化功能的专属测试主要只有 X86 一套：`table-check.ll`、
`table-extended-llt.ll`、`table-addrspace.ll`、`table-check-32bit.ll`、
`table-bench.ll`。AArch64 有运行时 `selfCheck()`，但没有 lit 级
`aarch64-gisel-table` 测试。

## 1. 单元测试：规则构建器与查询语义

### `llvm/unittests/CodeGen/GlobalISel/LegalizerInfoTest.cpp`

410 行，4 个 `TEST`：

- `RuleSets`
  - 直接构造 `LegalizerInfo`，用 `getActionDefinitionsBuilder(G_*)` 写规则；
  - 用 `EXPECT_ACTION(Action, TypeIdx, NewType, LegalityQuery(...))` 断言；
  - 覆盖 `legalFor`、`minScalar` / `maxScalar` / `clampScalar`、
    `minScalarOrElt` / `clampScalarOrElt`、`immediateIs` / `immInSet`、
    向量、指针、`s1`、非 2 的幂位宽等；
  - 这是 first-match 链语义和 builder/mutation 语义的最近层测试。
- `MMOAlignment`
  - 覆盖 `legalForTypesWithMemDesc` 的 alignment 匹配；
  - 验证 8/16/32 位 alignment 的接受/拒绝；
  - 验证最大 IR alignment（`1 << 29` 字节）不被截断。
- `MSVCDebugMiscompile`
  - 覆盖 `legalForCartesianProduct({S1}, {P0})` 的一个 MSVC x86_32
    debug 崩溃路径。
- `ImmediatesFromMI`
  - 用 `AArch64GISelMITest` 构造真实 `MachineInstr`；
  - 验证 `getAction(MI, MRI)` 把 `G_SEXT_INREG` 的 immediate 填进
    `LegalityQuery`；
  - 覆盖 `immIs` 的匹配与不匹配。

特点：这一层**不安装表**，测的是 `LegalizerInfo` 本身的链、谓词和
mutation。表化代码不能只靠这一层，因为它不会走 `LegalizeTable::lookup`。

## 2. 单元测试：单条 legalization 变换

### `llvm/unittests/CodeGen/GlobalISel/LegalizerHelperTest.cpp`

4599 行，102 个 `TEST_F`，全部基于 `AArch64GISelMITest`：

- 用 `DefineLegalizerInfo(Name, { ... })` 定义一个测试专用
  `LegalizerInfo`；
- 用 `MachineIRBuilder` 建 MIR；
- 调 `LegalizerHelper` 的具体变换；
- 用 `CheckMachineFunction(*MF, "CHECK: ...")` 比较期望 MIR。

覆盖面包括：

- `LowerRotates`、`LowerBitCounting*`；
- `WidenScalar*` / `NarrowScalar*`（CTLZ/CTTZ/CTPOP、UADDO/USUBO、
  SADDO/SSUBO、UADDE/USUBE、SADDE/SSUBE）；
- `FewerElements*` / `MoreElements*`；
- `LowerFNEG`、`LowerMinMax`、`LowerSelect`、`LowerBSWAP`；
- `Libcall*`（FExp2、FRem、FPow、FMa、FCeil、FFloor、FMinNum、
  FMaxNum、FSqrt、FRint、FNearbyInt）；
- `BitcastLoad` / `BitcastStore` / `BitcastSelect` / `BitcastBitOps`；
- `NarrowScalarExtract` / `NarrowScalarInsert` / `LowerInsert`；
- `WidenFreeze` / `NarrowFreeze` / `FewerElementsFreeze` /
  `MoreElementsFreeze`；
- extended LLT 的 `LowerIntrinsicRoundExtLLT`、`LowerFFloorExtLLT`、
  `LowerFMODFExtLLT`、`LowerFMinimumExtLLT`、`NarrowFPTOIExtLLT`。

特点：这一层测的是“规则说要做某个 action 之后，helper 真的做对了”，
不是规则选择本身。它对表化功能的价值是：如果表把 action 选错，helper
层不会发现；如果表选对了 action 但 mutation 的 `NewType` 错了，helper
层也可能把错误类型继续传下去。因此它不能替代表 vs 链的差分。

### `llvm/unittests/CodeGen/GlobalISel/LegalizerTest.cpp`

229 行，3 个 `TEST_F`：

- `BasicLegalizerTest`；
- `UnorderedArtifactCombiningTest`；
- `UnorderedArtifactCombiningManyCopiesTest`。

用 `legalizeMachineFunction(*MF, LI, ...)` 跑完整 legalizer，断言：

- `Result.FailedOn == nullptr`；
- `Result.Changed`；
- 最终 MIR 与期望一致。

这是最接近“真实 legalizer 跑一遍”的单元测试，但使用的是测试专用
`LegalizerInfo`，不是目标真实规则表。

## 3. 各目标的 MIR/lit 测试

### 规模

| 目标 | `legalize-*` / `legalizer-*` 文件数 |
|---|---:|
| AArch64 | 173 |
| AMDGPU | 191 |
| X86 | 72 |
| M68k | 6 |
| PowerPC | 1 |
| RISCV | 1 |

这些数字来自各 `llvm/test/CodeGen/<target>/GlobalISel` 目录的一层文件。
它们不是全部 GlobalISel 测试：AArch64 该目录共 808 个文件，AMDGPU 1148，
X86 227。

### 写法

典型形式：

```llvm
; RUN: llc -mtriple=aarch64 -run-pass=legalizer %s -o - | FileCheck %s
```

或：

```llvm
; RUN: llc -O0 -mtriple=x86_64-linux-gnu -run-pass=legalizer %s -o - \
; RUN:     | FileCheck %s --check-prefixes=CHECK,X64
; RUN: llc -O0 -mtriple=i386-linux-gnu -run-pass=legalizer %s -o - \
; RUN:     | FileCheck %s --check-prefixes=CHECK,X86
```

特点：

- 输入是手写或 `update_mir_test_checks.py` 生成的 MIR；
- 只跑 `legalizer` pass；
- 用 FileCheck 固定输出 MIR；
- 很多测试同时跑 64 位/32 位或不同 check prefix；
- 断言通常是“某条 `G_*` 变成什么”，而不是“规则号是多少”。

X86 的 72 个测试覆盖：

- 标量算术：add/sub/mul/and/or/xor/shl/lshr/ashr；
- 除法/取余：sdiv/udiv/srem/urem；
- 比较：icmp/fcmp；
- 转换：sext/zext/trunc/anyext、fpext/fptrunc、fptosi/sitofp；
- 位操作：ctpop/ctlz/cttz/bswap；
- 内存：memop scalar 32/64、store sse/avx/memtype、addrspace、
  x87 memdesc；
- 指针/控制：ptr-add 32/64、phi、brcond、select、freeze；
- 向量：add/sub/mul/and/or/xor 的 128/256/512 位变体。

AArch64 和 AMDGPU 的覆盖面更宽，包含大量 libcall、原子、向量、
intrinsic、exception 等。

### `-verify-machineinstrs` 的使用

X86 的 72 个 `legalize-*.mir` 中只有 8 个显式带
`-verify-machineinstrs`；整个 X86 GlobalISel 目录 227 个文件中有 120 个
带它。也就是说，legalizer lit 测试的主要 oracle 是 FileCheck 的 MIR
文本，machine verifier 不是每条 legalize 测试都跑。


### X86 直接覆盖率的量化

上面的“72 个测试覆盖了……”是按文件名和测试意图归纳的。把 `.td` 里的
规则集和实际 MIR 文本对起来看，覆盖面没有名字那么宽。

方法（全部可在本 worktree 复现）：

- `X86LegalizerInfo.td` 里有 61 个
  `def X86_* : LegalizeRuleSet<[...]>`，加上 `defm X86_LOAD` / `X86_STORE`
  两个 `X86_MemOp` 实例，共 **63 张表**；与 `table-check.ll` 注释里的
  “63 tables” 一致。
- 这 63 张表引用的 generic opcode 并集是 **115 个**（含 `G_LOAD` /
  `G_STORE`）。
- X86 目录下带 `-run-pass=legalizer` 的 MIR 文件是 **82 个**：72 个
  `legalize-*.mir`，另外 10 个是 `x86-legalize-*` / `x86_64-legalize-*`
  等命名。
- 在这 82 个文件的完整文本（输入 MIR 和 CHECK 行都算）中，出现过
  **60 / 115** 个规则 opcode；**55 个**没有出现过。
- 63 张表里，**21 张没有任何测试文件提到其任一 opcode**；
  **10 张只在一个文件里出现**。

“文本出现”是覆盖率的**上界**，不是执行覆盖：CHECK 行里出现某个
opcode，只说明某条期望输出提到了它；legalizer/helper 也可能间接生成
一个测试输入里没写的 opcode。反过来，某个 opcode 没在文本里出现，
基本可以确定没有直接构造它的 MIR 测试。这个口径足够回答“有没有一条
针对它的回归测试”，但不能回答“这条规则在真实 IR 里一定没跑过”。

零直接文本覆盖的 21 张表：

| 类别 | 表 |
|---|---|
| libcall / 浮点整组 | `X86_LROUND`（`G_LROUND`/`G_LLROUND`）、`X86_FCOS`（21 个 libcall 浮点 opcode）、`X86_FSQRT`、`X86_GET_ROUNDING`（`G_GET_ROUNDING`/`G_SET_ROUNDING`）、`X86_IS_FPCLASS`、`X86_INTRINSIC_ROUNDEVEN` |
| 整数 min/max | `X86_UMIN`（`G_UMIN`/`G_UMAX`/`G_SMIN`/`G_SMAX`） |
| 内存 / 转换 | `X86_SEXTLOAD`、`X86_ZEXTLOAD`、`X86_CONSTANT_POOL`、`X86_MEMCPY`（`G_MEMCPY`/`G_MEMMOVE`/`G_MEMSET`） |
| 向量 | `X86_BUILD_VECTOR`、`X86_EXTRACT` |
| 栈 / intrinsic / trap | `X86_DYN_STACKALLOC`、`X86_INTRINSIC`、`X86_TRAP`、`X86_INVOKE_REGION_START` |
| 其它 | `X86_SEXT_INREG`、`X86_FABS`、`X86_UITOFP`、`X86_FPTOUI` |

只有 1 个文件提到的 10 张表：

```text
X86_FNEG          legalize-fneg.mir
X86_SMULH         legalize-mul-scalar.mir   # 只在输出里出现，输入没有 G_SMULH
X86_BSWAP         legalize-bswap.mir
X86_CTLZ          legalize-leading-zeros.mir
X86_FPEXTLOAD     legalize-x87-memdesc.mir
X86_FPTRUNCSTORE  legalize-x87-memdesc.mir
X86_FCONSTANT     legalize-constant.mir
X86_FCMP          legalize-fcmp.mir
X86_SITOFP        x86_64-legalize-sitofp.mir
X86_FPTOSI        x86_64-legalize-fptosi.mir
```

注意 `X86_SMULH` 的两个 opcode 是 `G_SMULH` / `G_UMULH`；现有文件里
只有 `G_UMULH` 的直接输入，`G_SMULH` 只在期望输出中出现。这类“同一
规则集只测了一半”的情况，用文件名看是看不出来的。

### X86 特征与子目标覆盖

对 72 个 `legalize-*.mir` 逐项统计：

| 维度 | 有该标记的文件数 | 备注 |
|---|---:|---|
| `-mcpu` | 0 | `-mcpu` 只出现在 `table-check.ll` 的 x86-64/v2/v3/v4 |
| `-mattr` | 27 | 特征组合远少于 X86 的 SSE/AVX/AVX-512 子目标空间 |
| `target-features` | 3 | 只有 `legalize-mul-v128/v256/v512` |
| `i386` | 23 | 32 位有基本覆盖 |
| `x86_64` | 68 | 64 位主路径 |
| `gnux32` | 0 | x32 边界完全没测 |
| `-verify-machineinstrs` | 8 | 大多数 legalize 测试不跑 machine verifier |

整个 X86 GlobalISel 目录的 227 个文件里，`-mattr` 也只出现在 73 个文件，
`-mcpu` 只出现在 6 个文件。X86 的 legalizer 规则是按 subtarget 特性生成
不同表示的（SSE1/SSE2/AVX/AVX-512、popcnt/lzcnt/bmi 等），但 `legalize-*`
测试几乎不把“特征开/关”作为变量。

`X86LegalizerInfo.cpp` 对 x32 的注释已经指出：`is64Bit()` 为真而
`isTarget64BitLP64()` 为假的子目标会安装表，但 `p0:32` 查询会落回链。
现有测试没有一条覆盖这个边界。

### C++ 单测的边界

`llvm/unittests/CodeGen/GlobalISel` 下有 19 个文件，但 X86 目标专属的
C++ 单测是 **0**：

- `LegalizerHelperTest.cpp` 的 102 个 `TEST_F` 全部基于
  `AArch64GISelMITest`，用测试专用 `LegalizerInfo`；
- `LegalizerTest.cpp` 的 3 个 `TEST_F` 同样不是 X86 规则表；
- `LegalizerInfoTest.cpp` 只有 `MSVCDebugMiscompile` 的注释提到
  x86_32 的一个历史崩溃，没有构造 X86 `LegalizerInfo`；
- `X86LegalizerTable` / `LegalizerInfo::LegalizeTable` 没有直接的 C++
  单测。`Hit` / `Split` / `Outside` / `PointerVector` / `Declined` /
  `Untabulated` 这些路径，今天只能通过 X86 lit 间接碰到。

所以“C++ 测试不充分”在 X86 上不是感觉问题：底层 `LegalizerInfo` 的
通用单测是 AArch64 风格的，X86 表化代码的目标专属验证几乎全部压在 lit
上；而 lit 侧的规则覆盖率又只有 60/115（文本上界），其中 21 张表零直接
覆盖。

### 表化专属测试能补到哪

`table-check.ll` 的 `selfCheck()` 确实比 72 个文件名看起来强：它枚举
63 张表的生成域，并检查生成域内的表/链一致性，还检查 606 个 outside
witness。但它证明的是：

```text
生成器枚举到的域点：表 == 链
```

不是：

```text
真实 IR 经过 legalizer 的每个查询：表 == 链
```

`table-check.ll` 的 `VERIFY` 只跑 9 个 IR 函数，覆盖 add/mul/fadd/load/
store/vector/fptosi/add128/huge_scalar；`table-addrspace.ll` 和
`legalize-*-addrspace.mir` 补了 p272 的拒绝；其余 69 个 `legalize-*.mir`
没有把同一输入同时跑 chain/table 并逐字节比较——64 位下默认路径会经过表，
32 位下根本不安装表，但两种情况下都没有 on/off 对照。因此
`selfCheck` 能挡住“表在生成域内答错”，挡不住“某个真实 opcode 从来没
被测试构造过”或“域外查询/特征组合/回退路径没测”。

这条也解释了为什么“21 张表零文本覆盖”仍可能让 CI 全绿：它们的规则
可能在生成域的 representative 上被 `selfCheck` 问过，但没有一条真实
MIR 回归会在规则被改坏时指向具体的 opcode。

## 4. 规则集结构验证

### `LegalizerInfo::verify()`

实现位置：`llvm/lib/CodeGen/GlobalISel/LegalizerInfo.cpp:423`。

它在 `#ifndef NDEBUG` 下：

- 遍历 `FirstOp..LastOp` 的所有 generic opcode；
- 从 `MCInstrDesc` 计算每个 opcode 的 `NumTypeIdxs` / `NumImmIdxs`；
- 调 `verifyTypeIdxsCoverage()` / `verifyImmIdxsCoverage()`；
- 如果某个 opcode 的规则没有覆盖某个 type/imm index，收集并
  `report_fatal_error("ill-defined LegalizerInfo ...")`；
- 对 `Rules.empty()` 或“用户自定义谓词”的情况跳过，并在
  `-verbose-gisel-verify-legalizer-info` 下打印 `SKIPPED`。

目标 constructor 会调用 `verify(*ST.getInstrInfo())`，所以 assertions
build 启动时就会检查规则集结构。

### `legalizer-info-validation.mir`

只有两个目标有：

- `llvm/test/CodeGen/AArch64/GlobalISel/legalizer-info-validation.mir`
- `llvm/test/CodeGen/RISCV/GlobalISel/legalizer-info-validation.mir`

AArch64 的测试：

- `REQUIRES: asserts`；
- 用 `-debug-only=legalizer-info -verbose-gisel-verify-legalizer-info`；
- 对每个 opcode 检查 “first uncovered type/imm index: N, OK”；
- 对 `SKIPPED: user-defined predicate detected` 逐条要求有理由；
- 最后 `CHECK-NOT: ill-defined`。

X86 和 AMDGPU 没有同名的 `legalizer-info-validation.mir`。X86 表化功能
自带 `selfCheck()`，但它检查的是表 vs 链，不是 `LegalizerInfo::verify()`
的 type/imm index 覆盖度。

## 5. TableGen / 规则 DSL 测试

`llvm/test/TableGen` 下有 30 个 `legalizer-info*.td`（29 个带后缀的负例/结构例，加 1 个 `legalizer-info.td` 正例），用
`llvm-tblgen -gen-legalizer`、`-gen-legalizer-core` 或
`-gen-legalizer-table`。

### 正例

`legalizer-info.td`：

- 检查规则集生成的 builder 链顺序与 `.td` 顺序一致；
- 检查 feature 声明、predicate dag、类型列表、cartesian product、
  `legalForTypeWithAnyImm`；
- 例如 `Test_CTPOP` 生成
  `getActionDefinitionsBuilder(G_CTPOP).legalFor(...)...`。

### 负例

包括但不限于：

- `legalizer-info-bad-mutation.td`：mutation 方向拼错 → fatal；
- `legalizer-info-core-unknown-predicate.td`：Core IR 不认识的 leaf →
  fatal；
- `legalizer-info-arg-shape.td` / `legalizer-info-arity.td` /
  `legalizer-info-bad-builder.td`：builder 参数形状错误；
- `legalizer-info-duplicate.td` / `legalizer-info-empty.td` /
  `legalizer-info-missing-action.td`：规则集结构错误；
- `legalizer-info-table-escape.td`：opaque、类型列表选择、raw predicate、
  `typeDiffersFromMemDesc` 无 memory type 等“不表化”清单，以及 opcode
  映射；
- `legalizer-info-param-bounds.td`：条件 bound 的 residualization；
  `Test_UNPAREN` 是“不可表化”负控；
- `legalizer-info-float-fiber.td`：`FloatOK` 字段，决定某个类型轴是否
  允许问 FLOAT 拼写。

这一层是表化功能最接近“生成器契约”的测试，但它只检查生成物文本，
不跑 legalizer。

## 6. 表化功能的专属测试

### X86

- `table-check.ll`
  - 域差分：`points == compared`，`mismatches 0`，`skipped 0`；
  - 拒绝差分：`outside_checked > 0`，覆盖 unnamed width、unnamed lanes、
    scalable vector、nonzero addrspace、pointer vector、memdesc count、
    mem size/class；
  - 真实查询差分：`VERIFY` 跑 GlobalISel pipeline，要求 `checked > 0`；
  - `add128`：确保至少一个非 `Legal` 的答案形状；
  - `i65537`：class key 的 element 字段溢出回归。
- `table-extended-llt.ll`
  - X86 的 extended LLT 测试开关；
  - `fiber_max`、`fiber_skipped`；
  - `KIND` 检查 `i64`/`f64` 拼写；
  - 第三个 RUN 是答案缓存的负控（当前工作区删除该 flag 后已失效）。
- `table-addrspace.ll`
  - `p272` 必须走链，不能被 `p0` 类接住；
  - stats 断言 `addrspace 3`。
- `table-check-32bit.ll`
  - 32 位 subtarget 不安装表；
  - check 打印“未安装”而不是崩溃或报假数。
- `table-bench.ll`
  - benchmark 输出形状；
  - `-x86-gisel-table-once` 只答一个查询。

### 表 on/off 的端到端 diff

目前 X86 只有 3 个测试显式比较 table 与 chain：

- `legalize-store-memtype.mir`
  - 同时跑 `-x86-gisel-table=false` 和 `=true`；
  - 还额外把两次输出写到 `%t.table` / `%t.chain` 并 `diff`；
  - 这是现有测试里最接近“表 on/off 逐字节一致”的写法。
- `legalize-addrspace.mir`
  - 链和表都必须以同样方式拒绝 `p272` load。
- `legalize-store-addrspace.mir`
  - 链和表都必须以同样方式拒绝 `p272` store。

其余 69 个 X86 `legalize-*.mir` 只跑默认配置（表默认开启），没有和
chain-only 输出比较。

### AArch64

`grep -r aarch64-gisel-table llvm/test` 没有结果。AArch64 表有
`selfCheck()`（`AArch64LegalizerTable.cpp:435`）和
`-aarch64-gisel-table-verify`，但没有 lit 级测试调用它们。本地
`legalizer-table/README.md` 也记录了“AArch64 的 7 张表仍然没有 lit
级自检”。

## 7. 测试更新方式

大量 MIR 测试头部写着：

```text
# NOTE: Assertions have been autogenerated by utils/update_mir_test_checks.py
```

这意味着期望 MIR 可以脚本更新。优点是维护成本低；风险是“把回归当成
新基线”一并接受。表化改动导致的 MIR 变化必须人工 review，不能只看
测试是否绿。

## 8. 现有测试的强项与缺口

### 强项

- 规则构建器 / first-match / mutation 有直接的 C++ 单测；
- 每条 legalization 变换有大量 MIR 单测；
- 各目标有大量 `-run-pass=legalizer` 的 MIR 黄金测试；
- TableGen 有 30 个正/负例，生成器错误能早期暴露；
- X86 表有域差分、拒绝门、真实查询检查和 benchmark 负控。

### 缺口（相对表化功能）

1. **表 on/off diff 覆盖太少**：只有 3 个 X86 测试做 chain/table 对比，
   没有覆盖 `gen_ir.py` 的完整语料。
2. **AArch64 表没有 lit 级测试**：只有运行时 `selfCheck()`，CI 不会
   自动调用。
3. **X86 没有 `legalizer-info-validation.mir`**：`LegalizerInfo::verify`
   的 opcode 覆盖度检查没有 X86 的 lit 断言。
4. **没有随机/属性测试**：域差分是生成器给的有限域，真实查询只靠
   `-check` 和少量 IR；缺少对任意 `LegalityQuery` 的差分。
5. **没有每张表的覆盖率报告**：不知道哪些表在真实语料里从未命中。
6. **没有 per-function subtarget 测试**：没有验证同一个模块里不同
   `target-features` 的函数不会共享错误的表实例。
7. **没有 immediate / alignment / atomic ordering 的表化测试**：
   `G_SEXT_INREG`、`G_INSERT/G_EXTRACT`、非对齐 load/store、atomic
   load/store 在表路径上没有专门覆盖。
8. **没有 `LegalizerTable` 的 C++ 单测**：`Hit` / `Split` / `Outside` /
   `PointerVector` / `Declined` / `Untabulated` 这些 `Answer` 路径主要
   靠目标 lit 间接覆盖。
9. **没有 `Local` 单射检查**：今天 `Local` 恒等，未来压缩时没有测试
   兜底。
10. **当前 WIP 的测试已 red**：
    - `table-check.ll` 的 `VERIFY` 行仍要求 `cached ...`；
    - `table-extended-llt.ll` 仍引用已删除的
      `-x86-gisel-table-cache-extended`。
11. **并行 codegen 未验证**：`RulesOfCell` / `Cells` 是 lazy 填充的
    mutable 状态；如果 `LegalizerInfo` 可能被多个函数并行共享，需要
    TSan 压力测试或每函数实例化证明。当前测试没有覆盖这一点。

## 9. 一句话总结

现有 GlobalISel legalizer C++ 测试的主干是：

> **规则构建器单测 + 单条变换单测 + 各目标 MIR 黄金测试 +
> TableGen 生成器负例 + `LegalizerInfo::verify()` 的断言构建检查。**

表化功能在这套体系上额外加了 X86 的域差分/拒绝门/真实查询检查和
benchmark，但还没有把“表 vs 链”的差分系统性地铺到全部 legalize 语料、
AArch64 和随机查询空间上。
