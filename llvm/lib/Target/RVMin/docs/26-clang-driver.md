# 第 26 章：clang driver

## 目标

第 25 章只让 `clang -cc1` 可用；用户真正输入的是：

```bash
clang --target=rvmin-unknown-elf -march=rv32im -mabi=ilp32 -O2 -c demo.c
```

本章补 driver 层的四件事：

1. `--target=rvmin` 选中 **BareMetal** toolchain；
2. `-march/-mabi/-mcpu` 被翻译成 cc1 的 `-triple/-target-cpu/-target-feature/-target-abi`；
3. 集成汇编器（`-cc1as`）只收到 RVMin 认识的 feature；
4. 链接使用 `ld.lld -m elf32lriscv`。

本章结束时，`-c`、`-S`、`-c foo.s` 与 `-###`（含链接行）全部可用。

## 背景：driver 是按 arch 分派的

clang driver 不读 `TargetInfo` 的 feature 白名单，它先按 triple 做一组决策：

```text
Triple::rvmin
  ├─ ToolChain 选择        Driver.cpp: switch (Target.getArch())
  ├─ CPU                  CommonArgs.cpp: getTargetCPU()
  ├─ feature 向量          CommonArgs.cpp: getTargetFeatures()
  ├─ 链接器 emulation      CommonArgs.cpp: getLDMOption()
  └─ RISC-V 参数           Arch/RISCV.cpp + Clang::AddRISCVTargetArgs()
```

而 `Arch/RISCV.cpp` 里的每个函数都以 `Triple.isRISCV()` 为前提。第 24 章
刻意让 `isRISCV()` 对 rvmin 为 false，所以这些路径要么显式加 rvmin，
要么在进入前就拒绝——不能指望它们“顺带工作”。

## 代码地图

| 文件 | 改动 |
| --- | --- |
| `clang/lib/Driver/Driver.cpp` | `rvmin` → `BareMetal`；在 triple 改写处注明 rvmin 不得进入 |
| `clang/lib/Driver/ToolChains/CommonArgs.cpp` | frame pointer、`elf32lriscv`、CPU、feature 分派加 rvmin |
| `clang/lib/Driver/ToolChains/Clang.cpp` | 编译器/汇编器都调用 `AddRISCVTargetArgs`；拒绝 PIC/大代码模型/relax/RVV |
| `clang/lib/Driver/ToolChains/Arch/RISCV.cpp` | 断言放宽；rvmin 默认 `rv32i`/`ilp32`；拒绝 native 与 rv64；过滤 AS feature |
| `clang/lib/Basic/Targets/RVMin.{h,cpp}` | 接受 RVMin 的 5 个 CPU 名 |
| `clang/test/Driver/rvmin-target.c` | `-###` 正向断言（含链接行） |
| `clang/test/Driver/rvmin-invalid.c` | rv64/native/PIC/大代码模型/relax 负例 |

## 关键实现

### 1. BareMetal toolchain

```cpp
case llvm::Triple::riscv32:
case llvm::Triple::riscv64:
case llvm::Triple::riscv32be:
case llvm::Triple::riscv64be:
case llvm::Triple::rvmin:
  TC = std::make_unique<toolchains::BareMetal>(*this, Target, Args);
```

BareMetal 的 GCC/multilib 探测受 `isRISCVBareMetal` 保护，对 rvmin 会自然跳过；
它提供的 `-nostdlib`、静态链接与 `ld.lld` 路径正是我们要的。

同时，在 `Driver.cpp` 的 triple 改写块前加注释：

```cpp
// RVMin deliberately does not satisfy isRISCV(): this rewrite would turn
// `--target=rvmin -march=rv32i` into a riscv32 triple and silently select
// the RISCV backend.
if (Target.isRISCV()) { ... }
```

这不是新逻辑，而是把第 24 章的决策写进 driver 的“附近”，避免以后有人
“顺手”把 rvmin 加进 `isRISCV()`。

### 2. 默认值与诊断

`Arch/RISCV.cpp` 里三处必须以 rvmin 优先：

```cpp
// getRISCVArch: -mabi=ilp32 不能把 rvmin 变成 rv32imafdc
if (Triple.isRVMin()) {
  if (CPU == "generic-rv32-m") return "rv32im";
  ...
  return "rv32i";
}

// getRISCVABI: isRISCV32() 为 false，不能落到 lp64 分支
if (Triple.isRVMin()) {
  if (const Arg *A = Args.getLastArg(options::OPT_mabi_EQ))
    return A->getValue();
  return "ilp32";
}
```

以及进入 feature 展开前的校验：

- `-march=native` → `unsupported argument 'native' to option '-march' ...`；
- `-march=rv64i` → 同样报错（rvmin 只接受 XLEN=32 的 ISA 串）；
- `-march=rv32ic` 不在这里拒绝，而是让 cc1 的 `RVMinTargetInfo` 报
  `extension 'c' is not supported by rvmin`——driver 不做 ISA 白名单。

### 3. feature 向量必须在这里过滤

编译器路径有两层过滤：

```text
driver: getRISCVTargetFeatures → cc1 -target-feature
cc1:    RVMinTargetInfo::handleTargetFeatures → IR target-features / backend
```

但汇编器路径只有一层：

```text
driver: getRISCVTargetFeatures → cc1as -target-feature → MCSubtargetInfo
```

`-cc1as` 不创建 `TargetInfo`，所以第 25 章的过滤对它无效。如果 driver 原样
把 RISC-V 的 `-<ext>` 长列表传过去，`clang --target=rvmin -c foo.s` 会打印
几百行：

```text
'-c' is not a recognized feature for this target (ignoring feature)
```

因此 `getRISCVTargetFeatures` 对 rvmin 走一条短路径：

```cpp
if (!getArchFeatures(D, MArch, Features, Args))
  return;
llvm::erase_if(Features, [](StringRef F) {
  return F != "+m" && F != "+a" && F != "+zbb" && F != "+zbs";
});
return;
```

效果：

```text
-march=rv32i  → 无 +feature（i 是隐含基底）
-march=rv32im → "-target-feature" "+m"
```

### 4. `-mcpu`：RVMin 自己的 CPU 列表

`RVMin.td` 定义了 5 个 processor：

```text
generic-rv32, generic-rv32-m, generic-rv32-a, generic-rv32-zbb, generic-rv32-zbs
```

但 `RVMinTargetInfo` 继承的 `isValidCPUName` 来自 RISC-V，会拒绝这些名字。
本章覆盖：

```cpp
bool RVMinTargetInfo::isValidCPUName(StringRef Name) const {
  return llvm::is_contained({"generic-rv32", "generic-rv32-m", ...}, Name);
}
```

并让 `getRISCVArch` 把 CPU 名映射到 ISA 串（`generic-rv32-m` → `rv32im`），
这样宏（`__riscv_mul`）与后端选择（CPU 的 TableGen feature）保持一致。

### 5. 明确拒绝的组合

`Clang::AddRISCVTargetArgs` 里对 rvmin 直接诊断：

| 选项 | 原因 |
| --- | --- |
| `-fPIC/-fPIE` | 后端只有静态 `%hi/%lo` 绝对重定位 |
| `-mcmodel=medium/large` | 只有 small/medlow |
| `-mrelax` | 后端没有链接松弛 |
| `-mrvv-vector-bits=` | 没有 V |

错误消息形如：

```text
clang: error: unsupported option '-fPIC/-fPIE' for target 'rvmin-unknown-unknown-elf'
clang: error: unsupported argument 'large' to option '-mcmodel=' for target 'rvmin-unknown-unknown-elf'
```

### 6. 汇编器不传 build-attributes

`ClangAs::AddRISCVTargetArgs` 默认会传：

```text
-mllvm -riscv-add-build-attributes
```

RVMin 后端没有这个 emitter。对 rvmin 跳过：

```cpp
if (!Triple.isRVMin() &&
    Args.hasFlag(options::OPT_mdefault_build_attributes, ...)) { ... }
```

### 7. 链接器 emulation

```cpp
case llvm::Triple::riscv32:
case llvm::Triple::rvmin:
  return "elf32lriscv";
```

`getLDMOption` 只在直接调用 GNU-style 链接器时使用；BareMetal + lld 的组合
最终在 `-###` 里表现为：

```text
"/usr/bin/ld.lld" "-Bstatic" "-m" "elf32lriscv" ...
```

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B" clang llvm-readelf llvm-objdump

# 1) cc1 行
"$B/bin/clang" --target=rvmin-unknown-elf -march=rv32i -mabi=ilp32 \
  -### -c /tmp/t.c 2>&1 | grep -E 'triple|target-cpu|target-abi'

# 2) 对象与汇编
"$B/bin/clang" --target=rvmin-unknown-elf -march=rv32i -mabi=ilp32 -c /tmp/t.c -o /tmp/t.o
"$B/bin/llvm-readelf" -h /tmp/t.o | grep -E 'Class|Machine'
"$B/bin/clang" --target=rvmin-unknown-elf -march=rv32i -S /tmp/t.c -o /tmp/t.s

# 3) 集成汇编器
printf 'add a0, a0, a1\njalr zero, 0(ra)\n' > /tmp/foo.s
"$B/bin/clang" --target=rvmin-unknown-elf -march=rv32i -c /tmp/foo.s -o /tmp/foo.o

# 4) 链接行
"$B/bin/clang" --target=rvmin-unknown-elf -march=rv32i -mabi=ilp32 \
  -nostdlib -fuse-ld=lld -### /tmp/t.o -o /tmp/a.out 2>&1 | grep -A1 ld.lld

# 5) lit
"$B/bin/llvm-lit" -v "$W/clang/test/Driver/rvmin-target.c" \
                     "$W/clang/test/Driver/rvmin-invalid.c"
```

## 预期输出

```text
"-triple" "rvmin-unknown-unknown-elf"
"-target-cpu" "generic-rv32"
"-target-abi" "ilp32"
```

```text
  Class:   ELF32
  Machine: RISC-V
```

```text
"/usr/bin/ld.lld" "-Bstatic" "-m" "elf32lriscv" ...
```

feature 断言：

```text
-march=rv32i  → （没有 +m/+a/+c）
-march=rv32im → "-target-feature" "+m"
```

负例：

```text
-march=rv64i    → unsupported argument 'rv64i' to option '-march' for target 'rvmin-...'
-march=native   → unsupported argument 'native' to option '-march' for target 'rvmin-...'
-fPIC           → unsupported option '-fPIC/-fPIE' for target 'rvmin-...'
-mcmodel=large  → unsupported argument 'large' to option '-mcmodel=' for target 'rvmin-...'
-mrelax         → unsupported argument 'relax' to option '-mrelax' for target 'rvmin-...'
```

lit：

```text
PASS: Clang :: Driver/rvmin-invalid.c
PASS: Clang :: Driver/rvmin-target.c
  Passed: 2 (100.00%)
```

## 常见坑

1. **`isRISCV()` 为 false 的代价在 driver 里最集中。** ToolChain、CPU、
   feature、ABI、LDM 五张表都要显式加 rvmin；漏一处就会得到
   “看起来能编，但用了 riscv64 默认值”之类的错误。
2. **`getRISCVABI` 的默认分支是 `lp64`。** 因为它先判断 `isRISCV32()`，
   rvmin 为 false 就落到 64 位。必须在函数开头拦住。
3. **`getRISCVArch` 的 `-mabi=ilp32` 默认会返回 `rv32imafdc`。** 同理，
   rvmin 的默认必须在“按 mabi 猜 arch”之前返回 `rv32i`。
4. **汇编器路径绕过 TargetInfo。** 只在 `RVMinTargetInfo` 里过滤 feature
   不够；`-cc1as` 直接吃 driver 的向量，必须在那里裁到 4 个扩展。
5. **`-mcpu` 的合法性来自 TargetInfo，不是 TableGen。** RVMin.td 有
   `generic-rv32-m`，但如果 `isValidCPUName` 仍用 RISC-V 的列表，cc1 会在
   到达后端前就报 unknown CPU。
6. **`-###` 不创建 TargetMachine。** driver 测试应只断言命令行；真正的
   `-c`/`-S`/汇编在 `rvmin-clang-run.sh` 与第 28 章矩阵里验证。

## 练习

1. 去掉 `getRISCVTargetFeatures` 里的 AS 过滤，运行
   `clang --target=rvmin -c foo.s`，数一数警告行数，并解释为什么编译器路径
   没有同样的问题。
2. 给 `rvmin-invalid.c` 增加 `-mrvv-vector-bits=zvl` 与 `-mcpu=rocket-rv32`
   （RISC-V CPU 名）两个负例，确认前者被 driver 拒绝、后者被 TargetInfo 拒绝。
3. 用 `-###` 对比 `--target=riscv32-unknown-elf` 与 `--target=rvmin-unknown-elf`
   的 cc1 行，列出所有差异，并判断哪些差异是“必须的”，哪些只是历史默认值。
