# 第 0 章：target 骨架与注册

## 目标

在现有 LLVM 树里新增一个实验 target `rvmin`，满足：

1. `llc --version` 能列出 `rvmin`；
2. `llc -march=rvmin` 能选中 RVMin 的 `TargetMachine`；
3. 空模块能走完 codegen 流水线（`-filetype=null`）；
4. 不改 `Triple`、不改 clang、不新建构建目录。

本章结束时，非空函数会明确报错，这是预期的；指令选择从第 3 章开始。

## 背景

LLVM 的 target 注册有三层入口：

- `LLVMInitialize<Target>TargetInfo()`：注册 `Target` 对象与名字；
- `LLVMInitialize<Target>Target()`：注册 `TargetMachine` 工厂；
- `LLVMInitialize<Target>TargetMC()`：注册 `MCAsmInfo`、`MCInstrInfo`、
  `MCRegisterInfo`、`MCSubtargetInfo` 等 MC 组件。

`llc -march=rvmin` 的查找路径是
`TargetRegistry::lookupTarget(StringRef ArchName, Triple &, ...)`。
该函数先按 target 名字查找；如果名字没有对应的 `Triple::ArchType`，
就保留 clang 给的 `riscv32-unknown-unknown-elf` triple
（`llvm/lib/MC/TargetRegistry.cpp`）。因此 RVMin 可以：

- 用 `RegisterTarget<Triple::UnknownArch>` 注册；
- 复用 clang 的 RV32 数据布局 `e-m:e-p:32:32-i64:64-n32-S128`；
- 不碰 `Triple.h`、`Triple.cpp` 和 clang 的 `TargetInfo` 表。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `CMakeLists.txt` | 生成 TableGen `.inc`；定义 `LLVMRVMinCodeGen` |
| `RVMin.td` | target 顶层：处理器、寄存器、指令、`Target` 记录 |
| `RVMinRegisterInfo.td` | `x0`–`x31` 与 `GPR` 寄存器类 |
| `RVMinInstrFormats.td` | 最小指令基类（第 1 章扩展真实格式） |
| `RVMinInstrInfo.td` | 第 0 章只定义 `ADJCALLSTACKDOWN/UP` 伪指令 |
| `RVMinTargetMachine.{h,cpp}` | `TargetMachine`、数据布局、codegen 流水线入口 |
| `RVMinSubtarget.{h,cpp}` | `SubtargetInfo`、`InstrInfo`、`TargetLowering`、`FrameLowering` 的持有者 |
| `RVMinInstrInfo.{h,cpp}` | `TargetInstrInfo` 最小实现 |
| `RVMinRegisterInfo.{h,cpp}` | `TargetRegisterInfo` 最小实现 |
| `RVMinFrameLowering.{h,cpp}` | `TargetFrameLowering` 空实现 |
| `RVMinISelLowering.{h,cpp}` | `TargetLowering` 最小实现（只登记 `i32` → `GPR`） |
| `RVMinCodeGenPassBuilder.{h,cpp}` | 新 PM 的 codegen 流水线；第 0 章用一个报错 pass 占位 |
| `RVMinAsmPrinter.{h,cpp}` | `AsmPrinter` 外壳与 pass 定义 |
| `MCTargetDesc/RVMinMCTargetDesc.{h,cpp}` | MC 组件注册 |
| `MCTargetDesc/RVMinMCAsmInfo.{h,cpp}` | RV32 ELF 的汇编信息 |
| `TargetInfo/RVMinTargetInfo.{h,cpp}` | `Target` 对象与名字注册 |

## 关键实现

1. **名字注册**

   ```cpp
   RegisterTarget<Triple::UnknownArch> X(getTheRVMinTarget(), "rvmin", ...);
   ```

   模板默认 arch 就是 `UnknownArch`，与 LLVM 官方 “TOY” 教程的做法一致。

2. **数据布局**

   `TargetMachine` 继承 `CodeGenTargetMachineImpl`，构造时用 clang 实测的
   `e-m:e-p:32:32-i64:64-n32-S128`。这样 clang 生成的 IR 不需要改写。

3. **MC 组件必须齐全**

   `initAsmInfo()` 依次创建 `MCRegisterInfo`、`MCInstrInfo`、
   `MCSubtargetInfo`、`MCAsmInfo`，缺一个都会在构造时断言。
   第 0 章只注册这四个，外加一个 null target streamer；
   `MCCodeEmitter`/`MCAsmBackend`/`InstPrinter` 留给第 2 章。

4. **AsmPrinter 也必须注册**

   即使 `-filetype=null`，codegen 流水线也会创建 `AsmPrinter`。
   第 0 章用一个空壳：`emitInstruction` 未实现，但空模块不会调用它。

5. **两个容易踩的 TableGen 细节**

   - 目标无关伪指令 `PREALLOCATED_ARG`、`LOAD_STACK_GUARD` 等使用 `ptr_rc`，
     必须加 `defm : RemapAllTargetPseudoPointerOperands<GPR>;`。
   - 生成的 `*GenSubtargetInfo.inc` 使用 `LLVM_DEBUG`，
     `RVMinSubtarget.cpp` 必须先定义 `DEBUG_TYPE`。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

cmake -S "$W/llvm" -B "$B" -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=RVMin
ninja -C "$B" llc clang llvm-mc llvm-objdump llvm-readelf FileCheck

"$B/bin/llc" --version | grep -w rvmin

printf 'target triple = "riscv32-unknown-unknown-elf"\n' > /tmp/rvmin-empty.ll
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -filetype=null \
  /tmp/rvmin-empty.ll
echo $?
```

预期输出：

```text
    rvmin      - RVMin (RV32I minimal) [experimental]
0
```

非空函数（第 0 章预期失败）：

```text
LLVM ERROR: RVMin: instruction selection is not implemented yet
```

## 常见坑

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| TableGen 报 `missing target override ... ptr_rc` | 标准伪指令未重映射 | 加 `RemapAllTargetPseudoPointerOperands<GPR>` |
| `DEBUG_TYPE was not declared` | 生成的 subtarget `.inc` 用了 `LLVM_DEBUG` | 在 cpp 里 `#define DEBUG_TYPE` |
| `ADJCALLSTACKDOWN is not a member of RVMin` | 只包含了 `GET_INSTRINFO_HEADER`，枚举在 `GET_INSTRINFO_ENUM` | 在 cpp 里包含 `RVMin.h` |
| `getFrameLowering` 用到不完整类型 | 生成代码需要完整 `TargetSubtargetInfo` | 在 cpp 里包含 `RVMinTargetMachine.h` |
| clang 报 `undefined symbol ... SampleProfileFile` | 重配置后只构建了 `llc`，clang 的动态库是旧的 | 同时 `ninja clang` |
| 非空函数断言崩溃 | 没有指令选择 | 第 3 章实现；第 0 章用显式报错占位 |

## 练习

1. 把 target 名改成别的名字（如 `rvminx`），验证 `llc -march=` 与
   `-mtriple=riscv32-unknown-elf` 的组合仍然工作。
2. 去掉 `RemapAllTargetPseudoPointerOperands`，记录 TableGen 的具体报错。
3. 用 `llc --version` 与 `llc -march=rvmin -mattr=help` 观察实验 target 的注册信息。

## 下一章

第 1 章加入真实 RV32I 指令定义：R/I/S/B/U/J 六种格式、`x0`–`x31` 的 ABI 别名、
以及 `ADDI/ADD/SUB/LUI/AUIPC/JAL/JALR` 等核心指令的 TableGen 描述。
