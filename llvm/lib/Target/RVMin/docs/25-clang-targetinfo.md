# 第 25 章：clang TargetInfo

## 目标

第 24 章让 `Triple` 认识了 `rvmin`，但 clang 仍会在创建 TargetInfo 时失败：

```text
$ clang -cc1 -triple rvmin-unknown-elf -E -x c /dev/null
error: unknown target triple 'rvmin-unknown-unknown-elf'
```

本章补上 `clang/lib/Basic/Targets/RVMin.{h,cpp}`，让 **cc1 层**可用：

```text
$ clang -cc1 -triple rvmin-unknown-elf -target-abi ilp32 -O2 -emit-obj arith.c -o arith.o
$ llvm-readelf -h arith.o | grep -E 'Class|Machine'
  Class:   ELF32
  Machine: RISC-V
```

driver（`clang --target=rvmin`、`-march=`、`-mabi=`）留给第 26 章。

## 背景：TargetInfo 是 clang 的“目标合同”

clang 的代码生成只依赖 `TargetInfo` 回答的这些问题：

- 指针/整数/`long double` 的宽度与对齐；
- 调用约定、参数寄存器、返回寄存器；
- 预定义宏（`__riscv`、`__riscv_xlen`、`__riscv_float_abi_soft`）；
- 目标 builtin 列表；
- 哪些 feature 合法。

RVMin 与 riscv32 在这些问题上的答案**几乎完全一样**（ILP32、soft-float ABI、
同样的寄存器与 data layout）。真正不同的是 feature 面：RVMin 只有
RV32I + 可选 M/Zbb/Zbs/A，没有 C/F/D/V。

所以本章不新写一个 2000 行的 TargetInfo，而是：

```cpp
class RVMinTargetInfo : public RISCV32TargetInfo { ... };
```

只覆盖“不一样”的五个点。

## 代码地图

| 文件 | 改动 |
| --- | --- |
| `clang/lib/Basic/Targets/RVMin.h` | 新文件：`RVMinTargetInfo : RISCV32TargetInfo` |
| `clang/lib/Basic/Targets/RVMin.cpp` | 新文件：宏、builtins、ABI、feature 校验/过滤 |
| `clang/lib/Basic/Targets.cpp` | include + `case llvm::Triple::rvmin` |
| `clang/lib/Basic/CMakeLists.txt` | 编译 `Targets/RVMin.cpp` |
| `clang/lib/CodeGen/CodeGenModule.cpp` | `target-abi` module flag 的白名单加 `isRVMin()` |
| `clang/test/Preprocessor/rvmin-target.c` | 宏与负例 |
| `clang/test/CodeGen/rvmin-abi.c` | IR 的 triple/data layout/target-abi |

## 关键实现

### 1. 复用 RISCV32 的布局与 ABI

构造函数只关掉两件事：

```cpp
RVMinTargetInfo(const llvm::Triple &Triple, const TargetOptions &Opts)
    : RISCV32TargetInfo(Triple, Opts) {
  HasRISCVVTypes = false;  // 没有 RVV 向量类型
  TLSSupported = false;    // 后端没有 TLS
}
```

`IntPtrType`/`SizeType`/`PtrDiffType`、`LongDoubleWidth = 128`、寄存器名、
调用约定全部继承。data layout 与 riscv32 相同：

```text
e-m:e-p:32:32-i64:64-n32-S128
```

### 2. 不暴露任何 target builtin

```cpp
llvm::SmallVector<Builtin::InfosShard> getTargetBuiltins() const override {
  return {};
}
```

否则 clang 会接受 `__builtin_riscv_*`（CSR、向量等），而后端没有对应指令，
只能在 SelectionDAG 里崩溃或悄悄生成错误代码。宁可让前端直接报
“use of undeclared identifier”。

### 3. ABI 只接受 ilp32

```cpp
bool RVMinTargetInfo::setABI(const std::string &Name) {
  if (Name != "ilp32")
    return false;
  ABI = Name;
  return true;
}
```

`lp64*` 是 64 位 ABI，`ilp32f/ilp32d` 需要 F/D 扩展与硬浮点寄存器；
RVMin 都不支持，直接让 clang 报 `unknown target ABI`。

### 4. feature 面：白名单 + 丢弃 + 诊断

`handleTargetFeatures` 分三步：

1. **先让 RISCV32TargetInfo 解析**，这样 `+m` 展开出的隐含扩展
   （`zmmul`、`zalrsc`、`zaamo`）都能被看到；
2. **按白名单校验**：只允许 `i/m/a/zbb/zbs` 与上述隐含扩展，
   其余（`c/f/d/v/zfh/zve*`……）报
   `extension 'c' is not supported by rvmin`；
3. **只保留后端认识的 `+feature`**：
   `+i`、`+zicsr`、`+zifencei`、`+relax` 以及所有 `-<ext>` 都从
   `Features` 里删掉。

第 3 步不是洁癖。`Features` 会被写进每个函数的
`"target-features"` 属性；driver 在 `-march=rv32i` 时会传一长串
`-target-feature -c`、`-target-feature -v`……如果原样保留，后端每次
编译都会打印几十条：

```text
'-c' is not a recognized feature for this target (ignoring feature)
```

过滤后，`-march=rv32i` 的 IR 没有 `target-features`，
`-march=rv32im` 的 IR 恰好是 `"target-features"="+m"`。

### 5. 两个 clang 侧的连带修改

**`+32bit`**：`RISCVTargetInfo::initFeatureMap` 会设置
`Features["32bit"] = true`，供 `__has_feature(32bit)` 使用。这个名字也会
被序列化进 `target-features`，但 RVMin.td 没有这个 feature，于是每个函数
都会出现：

```text
'+32bit' is not a recognized feature for this target (ignoring feature)
```

RVMin 的 `initFeatureMap` 在调用基类后 `Features.erase("32bit")`。

**`target-abi` module flag**：`CodeGenModule.cpp` 只在
`T.isRISCV()` 等白名单里写 `!{i32 1, !"target-abi", !"ilp32"}`。
`isRISCV()` 对 rvmin 为 false（第 24 章的决策），所以必须显式加
`T.isRVMin()`，否则 IR 里没有 ABI，LTO/`llc` 无法还原 ILP32。

### 6. `__rvmin` 宏

```cpp
void RVMinTargetInfo::getTargetDefines(const LangOptions &Opts,
                                       MacroBuilder &Builder) const {
  RISCVTargetInfo::getTargetDefines(Opts, Builder);
  Builder.defineMacro("__rvmin");
}
```

`__riscv*` 宏全部保留（ABI 相同），额外加一个 `__rvmin` 让源码能区分
“真的是 riscv32”还是“教学后端”。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B" clang llvm-readelf llvm-objdump

# 1) 宏
"$B/bin/clang" -cc1 -triple rvmin-unknown-elf -E -dM -x c /dev/null \
  | grep -E '__riscv|__rvmin'

# 2) 对象
"$B/bin/clang" -cc1 -triple rvmin-unknown-elf -target-abi ilp32 -O2 \
  -emit-obj "$W/llvm/lib/Target/RVMin/scripts/e2e/arith.c" -o /tmp/arith.o
"$B/bin/llvm-readelf" -h /tmp/arith.o | grep -E 'Class|Machine'

# 3) IR
"$B/bin/clang" -cc1 -triple rvmin-unknown-elf -target-abi ilp32 -O2 \
  -emit-llvm "$W/llvm/lib/Target/RVMin/scripts/e2e/arith.c" -o /tmp/arith.ll
grep -E 'target datalayout|target triple|target-abi' /tmp/arith.ll

# 4) lit
"$B/bin/llvm-lit" -v "$W/clang/test/Preprocessor/rvmin-target.c" \
                     "$W/clang/test/CodeGen/rvmin-abi.c"
```

## 预期输出

```text
#define __riscv 1
#define __riscv_arch_test 1
#define __riscv_cmodel_medlow 1
#define __riscv_float_abi_soft 1
#define __riscv_i 2001000
#define __riscv_misaligned_avoid 1
#define __riscv_v_intrinsic 1000000
#define __riscv_xlen 32
#define __rvmin 1
```

没有 `__riscv_compressed`、没有 `__riscv_flen`。

```text
  Class:   ELF32
  Machine: RISC-V
```

```text
target datalayout = "e-m:e-p:32:32-i64:64-n32-S128"
target triple = "rvmin-unknown-unknown-elf"
!0 = !{i32 1, !"target-abi", !"ilp32"}
```

负例：

```text
$ clang -cc1 -triple rvmin-unknown-elf -target-abi lp64 ...
error: unknown target ABI 'lp64'

$ clang -cc1 -triple rvmin-unknown-elf -target-feature +c ...
error: invalid feature combination: extension 'c' is not supported by rvmin

$ clang -cc1 -triple rvmin-unknown-elf -target-feature +v ...
error: invalid feature combination: extension 'f' is not supported by rvmin
```

> `+v` 先报 `f`，因为 ISA-info 自动把 V 依赖的 F 一起打开；诊断仍然是否定的。

lit：

```text
PASS: Clang :: CodeGen/rvmin-abi.c
PASS: Clang :: Preprocessor/rvmin-target.c
  Passed: 2 (100.00%)
```

## 常见坑

1. **不要继承 RISCV 的 builtins。** 覆盖为空是第一优先级；否则
   `__builtin_riscv_csr_read` 这类调用能过前端，到后端才炸。
2. **`+32bit` 是隐式注入的。** 它来自 `RISCVTargetInfo::initFeatureMap`，
   不是 driver 传的；不在 RVMin 的 feature map 里删掉，每个函数都会警告。
3. **`+m` 不是只有 `m`。** 本版 ISA-info 中 `m` 隐含 `zmmul`；白名单必须
   允许隐含扩展，否则合法的 `-march=rv32im` 会被自己拒绝。
4. **IR 的 `target-features` 来自 `TargetOpts.Features`。** driver 会传完整
   的负数 feature 列表；必须在 `handleTargetFeatures` 里过滤，否则警告刷屏，
   而且 `llc` 读 IR 时也会看到一堆不认识的 feature。
5. **`target-abi` 不是自动的。** `CodeGenModule` 的白名单按 `isRISCV()`
   判断；rvmin 为 false 时必须显式加，否则 IR 缺 ABI，链接/LTO 阶段会退回
   默认值。

## 练习

1. 给 `RVMinTargetInfo` 加一个 `__rvmin_isa` 宏，按启用的扩展输出
   `"rv32i"` / `"rv32im"` / `"rv32im_zbb"`，并在 lit 里覆盖 3 种组合。
2. 故意把 `getTargetBuiltins()` 改回基类实现，编译
   `int f() { return __builtin_riscv_csr_read(0); }`，记录它在前端/后端分别
   如何失败，并解释为什么“前端拒绝”更好。
3. 用 `-target-feature +m,+zbb` 生成 IR，检查 `target-features` 属性的
   顺序与内容；再把它喂给 `llc -march=rvmin`，确认选择到 `mul`/`andn`。
