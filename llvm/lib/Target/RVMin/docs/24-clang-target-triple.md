# 第 24 章：让 triple 认识 rvmin

## 目标

前三篇的入口一直是 `llc -march=rvmin -mtriple=riscv32-unknown-elf`：
后端按名字被选中，但 LLVM 的 `Triple` 里根本没有 `rvmin` 这个 arch。
本章只做一件事：**给 `Triple` 增加 `ArchType::rvmin`**，让
`llc -mtriple=rvmin-unknown-elf`、`llvm-mc -triple=rvmin` 以及后续
clang 的 `--target=rvmin-unknown-elf` 有一个真正的 arch 可以落。

本章结束时：

```text
$ llc -march=rvmin -mtriple=rvmin-unknown-elf -O2 demo.ll -o -
    add a0, a0, a1
    jalr zero, 0(ra)
```

但 `clang --target=rvmin-unknown-elf` 仍然会失败——那是第 25、26 章的事。

## 背景：为什么必须有 `Triple::rvmin`

`clang` 创建 `TargetMachine` 走的是 triple 查找，而不是 `-march`：

- `clang/lib/CodeGen/BackendUtil.cpp`：`TargetRegistry::lookupTarget(Triple, Error)`；
- `llvm/lib/MC/TargetRegistry.cpp`：按 `ArchMatchFn(TT.getArch())` 匹配后端；
- RVMin 之前注册的是 `RegisterTarget<Triple::UnknownArch>`，只能被
  `llc -march=rvmin` 这种“按名字选择”的路径命中。

所以“一等 target”的第一层不是 clang 代码，而是 triple 枚举本身。
本章把这一层补齐，并且**只补这一层**。

### 一个必须坚持的决策：`isRISCV()` 保持 false

最省事的写法是把 `rvmin` 塞进 `isRISCV()`/`isRISCV32()`。不能这么做：

```cpp
// clang/lib/Driver/Driver.cpp
if (Triple.isRISCV() && !Args.getLastArgValue(options::OPT_march_EQ).empty())
  Triple.setArch(Triple::riscv32);
```

只要 `isRISCV()` 为真，`clang --target=rvmin -march=rv32i` 就会被驱动悄悄改回
`riscv32`，最终选中 `llvm/lib/Target/RISCV`。门禁会全绿，但跑的不是我们的后端。

因此本章的不变量是：

> **rvmin 是“借用 RISC-V ABI/对象格式的独立 arch”，不是 riscv32 的子类型。**

代价是：所有“只对 RISC-V 生效”的隐式白名单都不会自动包含 rvmin。
下面“关键实现”里的三处连带修改，都是这个决策的直接后果。

## 代码地图

| 文件 | 改动 |
| --- | --- |
| `llvm/include/llvm/TargetParser/Triple.h` | 枚举加 `rvmin`；新增 `isRVMin()`；`isRISCV*` 不变 |
| `llvm/lib/TargetParser/Triple.cpp` | 名称/解析/格式/位宽/端序/异常模型/float ABI/long double 等 12 处 |
| `llvm/lib/TargetParser/TargetDataLayout.cpp` | `computeRISCVDataLayout` 接受 rvmin；`computeDataLayout` 加 case |
| `llvm/lib/Target/RVMin/TargetInfo/RVMinTargetInfo.cpp` | `RegisterTarget<Triple::rvmin>` |
| `llvm/include/llvm/IR/RuntimeLibcalls.td` | 新增 `IsRVMin` 谓词与 `RVMinSystemLibrary` |
| `llvm/unittests/TargetParser/TripleTest.cpp` | 新增 `TEST(Triple, RVMin)` |

## 关键实现

### 1. 枚举与 `isRVMin()`

```cpp
// Triple.h
riscv64be,   // RISC-V (64-bit, big endian): riscv64be
rvmin,       // RVMin: minimal in-tree RV32I teaching backend
sparc,       // Sparc: sparc
```

`LastArchType` 仍然是 `ve`——新枚举插在中间，不改变哨兵。

```cpp
bool isRVMin() const { return getArch() == Triple::rvmin; }
```

`isRISCV()`、`isRISCV32()`、`isRISCV64()` 一个字都不改。

### 2. Triple.cpp 的 12 处

按“漏一处会怎样”排序：

| 位置 | 漏掉的后果 |
| --- | --- |
| `getArchTypeName` / `getArchTypeForLLVMName` / `parseArch` | 字符串与枚举不能互转，`--target=rvmin` 解析成 `UnknownArch` |
| `getDefaultFormat` | 默认不是 ELF，对象文件/汇编器行为异常 |
| `getArchPointerBitWidth` | 指针宽度错，data layout 与 ABI 全错 |
| `get32BitArchVariant` | `-m32`/`get32BitArchVariant()` 变成 `UnknownArch` |
| `get64BitArchVariant` / `getBigEndianArchVariant` | 应明确返回 `UnknownArch`，否则会造出 `rvmin64`/`rvminbe` 这类不存在的变体 |
| `isLittleEndian()` | **白名单漏掉就变 big-endian**，编码全部反过来 |
| `getDefaultExceptionHandling` | 不是 DwarfCFI，`.cfi_*` 与 `.eh_frame` 整段消失 |
| `getDefaultFloatABI` | 默认 Hard float，与 `ilp32` 矛盾 |
| `getDefaultLongDoubleFormat` | `long double` 的 libcall 选择错 |
| `getArchTypePrefix` | 该 switch **没有 default**，漏掉就是未定义行为（clang 每个 builtin 调用都会走） |

最后两条不是计划里最初列出的，是实测踩出来的，见“常见坑”。

### 3. 数据布局

```cpp
// TargetDataLayout.cpp
assert((TT.isRISCV32() || TT.isRVMin()) && "Unsupported triple for RISC-V DL");
...
case Triple::rvmin:
  return computeRISCVDataLayout(Layout, TT);
```

结果与 riscv32 完全一致：

```text
e-m:e-p:32:32-i64:64-n32-S128
```

### 4. 注册 arch

```cpp
// RVMinTargetInfo.cpp
RegisterTarget<Triple::rvmin>(X, "rvmin", "RVMin (32-bit little endian)", ...);
```

注册成 `rvmin` 后，`llc -march=rvmin` 仍然可用：`lookupTarget` 在按名字
找到后端后，会用 `getArchTypeForLLVMName("rvmin")` 把 triple 的 arch 改成
`rvmin`。这也意味着测试里写的 `-mtriple=riscv32-unknown-elf` 会被改写成
`rvmin`，行为与之前一致。

### 5. 两个隐式白名单：运行库与 CFI

`Triple::isRISCV()` 为 false 之后，有两处“只认 RISC-V”的表不会自动带上 rvmin：

**运行库**（`RuntimeLibcalls.td`）：

```tablegen
def IsRVMin : LibcallPredicate<[{TT.isRVMin()}]>;
def isRVMin : RuntimeLibcallAvailability<(all_of IsRVMin)>;
def RVMinSystemLibrary
    : SystemRuntimeLibrary<isRVMin,
                           ["compiler-rt", "libatomic", "llvm-client-runtime",
                            "libm", "libc", "unwind"]>;
```

漏掉它，`llc -O1` 会在 legalizer 报：

```text
error: no libcall available for mul
error: no libcall available for sdiv
```

因为 `MUL`/`SDIV` 要展开成 `__mulsi3`/`__divsi3`，而 rvmin 没有任何
runtime library 可用。

**异常模型**（`Triple.cpp`）：把 `rvmin` 加进 DwarfCFI 的 arch switch。
漏掉它，`cfi.ll`、`large-frame.ll`、`tail-call.ll` 会同时失败，现象是
汇编里一条 `.cfi_*` 都没有——不是 CFI 指令没生成，而是
`AsmPrinter::emitCFIInstruction` 按 `ModuleExceptionModel` 直接跳过了。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B" llc llvm-mc llvm-readelf FileCheck TargetParserTests
"$B/unittests/TargetParser/TargetParserTests" --gtest_filter='Triple.RVMin'

# 1) 后端按 triple 选
"$B/bin/llc" -march=rvmin -mtriple=rvmin-unknown-elf -O2 /tmp/rvmin-smoke.ll -o -

# 2) 汇编器按 triple 选
"$B/bin/llvm-mc" -triple=rvmin /tmp/rvmin-mattr.s -show-encoding

# 3) 既有后端门禁
"$B/bin/llvm-lit" -s "$W/llvm/test/MC/RVMin" "$W/llvm/test/CodeGen/RVMin"
"$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"
```

## 预期输出

```text
[ RUN      ] Triple.RVMin
[       OK ] Triple.RVMin (0 ms)
[  PASSED  ] 1 test.

    add a0, a0, a1
    jalr zero, 0(ra)

	addi a0, a0, 1                          # encoding: [0x13,0x05,0x15,0x00]

Total Discovered Tests: 24
  Passed: 24 (100.00%)

PASS 1/6: target registered
PASS 2/6: lit (CodeGen + MC)
PASS 3/6: encodings
PASS 4/6: disassembler
PASS 5/6: e2e
PASS 6/6: 128 llc runs with -verify-machineinstrs
ALL GATES PASS
```

`Triple.RVMin` 单测覆盖：

- `Triple("rvmin-unknown-elf")`：arch/archName/`isRVMin()`/`isRISCV()` 为 false；
- little-endian、32 位、ELF；
- data layout `e-m:e-p:32:32-i64:64-n32-S128`；
- `FloatABI::Soft`、`LongDoubleFormat::IEEEquad`；
- `get32BitArchVariant()` 仍是 rvmin；64 位与 BE 变体是 `UnknownArch`；
- `getArchTypeForLLVMName("rvmin")` 与 `getArchTypeName(rvmin)` 互逆。

## 常见坑

1. **`isRISCV()` 不能顺手改成 true。** 见“背景”。这不是风格问题：
   driver 会把 triple 改回 riscv32，整个 part 3 的验收会变成假绿。
2. **白名单是隐式的。** `isLittleEndian()` 是“列出来才算 LE”，漏掉 rvmin
   不会报错，只会安静地按 big-endian 处理；`getArchTypePrefix()` 的 switch
   则连 default 都没有，漏掉是 UB。加 arch 时要把 `Triple.cpp` 里所有
   `case riscv32:` 附近都过一遍。
3. **运行库谓词不是 `isRISCV()` 的语法糖。** 它由 TableGen 生成，
   单独维护；`isRISCV()` 为 false 时必须在 `RuntimeLibcalls.td` 显式登记，
   否则 `-O1` 才暴露 `no libcall available`。
4. **CFI 失败不一定在 CFI 代码里。** 三个测试同时丢 `.cfi_*`，根因是
   `getDefaultExceptionHandling()` 返回 `None`；先查 triple 的异常模型，
   再查 frame lowering。
5. **测试里的 `-mtriple=riscv32-unknown-elf` 不用改。** `llc -march=rvmin`
   会按后端名把 arch 改成 rvmin；保留 riscv32 拼写反而验证了这条兼容路径。

## 练习

1. 把 `rvmin` 临时加进 `isRISCV()`，只跑
   `clang -### --target=rvmin-unknown-elf -march=rv32i -c t.c`，
   观察 `-triple` 变成了什么；然后改回来，解释为什么这会让 AC1–AC10 全部失真。
2. 给 `Triple.RVMin` 补一条 `getDefaultExceptionHandling() == DwarfCFI` 的断言，
   并故意注释掉 `Triple.cpp` 里的 `case rvmin`，确认 `cfi.ll` 会失败。
3. 用 `git grep -n "case riscv32" llvm/lib/TargetParser/Triple.cpp` 列出所有
   RISC-V 分支，逐个判断 rvmin 应该“跟随”“显式拒绝”还是“无关”，做成一张表。
