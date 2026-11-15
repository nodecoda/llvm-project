# 第 16 章：大栈帧与 RegScavenger

## 目标

第 6 章留下了一个明确报错的硬限制：帧内偏移超过 12 位就 `report_fatal_error`。
本章把它变成“能算出来”：

```c
int big(int x) {
  volatile char buf[3072];      // 3 KiB，远超 2047
  buf[3000] = 7;
  return buf[3000];
}
```

完成本章后：

1. `LW/SW/LB/SB/...` 的偏移可以任意大（帧大小只受 32 位地址空间限制）；
2. `sp` 的调整也能处理大帧（第 6 章已有 `LUI+ADDI`，本章把它抽成可复用的 helper）；
3. 理解 `eliminateFrameIndex` 的第 4 个参数 `RegScavenger *RS` 为什么长期没用、
   以及“寄存器分配之后还要再借一个寄存器”这件事该怎么做；
4. 知道 `requiresFrameIndexScavenging` / `scavengeFrameVirtualRegs` /
   emergency spill slot 三者的分工；
5. 3 KiB 帧（以及 9 层嵌套）的程序能在 QEMU 里跑出正确结果。

## 背景：为什么 2 KiB 是硬限制

RV32I 的 load/store 只有一个 12 位有符号立即数：

```text
lw  rd, imm(rs1)      imm ∈ [-2048, 2047]
```

第 6 章的选择是“拒绝而不是算错”：

```text
LLVM ERROR: RVMin: frame offset 3000 does not fit in 12 bits
             (frame larger than 2 KiB is not supported yet)
```

要支持更大的帧只有两条路：

| 方案 | 做法 | 代价 |
| --- | --- | --- |
| 帧指针 | 让 `s0` 指向帧中间，把远访问变成近访问 | 少一个寄存器、CFI 要改、`-O0` 全变 |
| 物化地址 | 把 `sp+offset` 算进一个临时寄存器，再用 `0(临时)` 访问 | 每次远访问多 3 条指令 |

上游 RISC-V 的做法是物化地址（`eliminateFrameIndex` 里建虚拟寄存器 +
`adjustReg`）。本章照这个思路做，但把重点放在一个关键难点上：**这段代码跑在
寄存器分配之后**，此时没有“空闲寄存器”可以随便用。

## 代码地图

| 文件 | 本章改动 |
| --- | --- |
| `RVMinFrameLowering.h/.cpp` | 抽出 `materializeConstant` / `materializeAddress`；新增 `processFunctionBeforeFrameFinalized` 保留 emergency spill slot；序言/收尾改用新 helper |
| `RVMinRegisterInfo.h` | `requiresRegisterScavenging`、`requiresFrameIndexScavenging` 返回 `true` |
| `RVMinRegisterInfo.cpp` | `eliminateFrameIndex`：偏移超 12 位时物化地址，不再报错 |
| `llvm/test/CodeGen/RVMin/large-frame.ll` | 3 KiB 帧 + 非叶函数（callee-saved 槽也变远） |
| `scripts/rt/large-frame.c` | QEMU 运行门禁：3 KiB 帧、9 层嵌套、`-O0`/`-O2` |
| `scripts/rvmin-run.sh` | 新增 large-frame 运行段 |
| `llvm/test/CodeGen/RVMin/stack.ll` | 第 6 章的 `big` 期望值随 emergency slot 更新（见“常见坑”） |

## 关键实现

### 1. 先把常量物化抽出来

第 6 章的 `adjustStackPointer` 里已经有一段 `LUI+ADDI`，但它内联在
`sp` 调整的分支里。本章两个调用点（序言/收尾、帧索引消除）需要同一件事，
所以把它提到 `RVMinFrameLowering` 的静态方法：

```cpp
// RVMinFrameLowering.cpp
void RVMinFrameLowering::materializeConstant(MachineBasicBlock &MBB,
                                             MachineBasicBlock::iterator I,
                                             const RVMinInstrInfo &TII,
                                             const DebugLoc &DL, Register Dst,
                                             int64_t Value,
                                             MachineInstr::MIFlag Flag) {
  if (isInt<12>(Value)) {
    BuildMI(MBB, I, DL, TII.get(RVMin::ADDI), Dst)
        .addReg(RVMin::X0).addImm(Value).setMIFlag(Flag);
    return;
  }
  int64_t Hi = (Value + 0x800) >> 12;
  int64_t Lo = Value - (Hi << 12);
  BuildMI(MBB, I, DL, TII.get(RVMin::LUI), Dst).addImm(Hi).setMIFlag(Flag);
  if (Lo)
    BuildMI(MBB, I, DL, TII.get(RVMin::ADDI), Dst)
        .addReg(Dst).addImm(Lo).setMIFlag(Flag);
}
```

`materializeAddress` 在它之上加一条 `ADD`：

```cpp
void RVMinFrameLowering::materializeAddress(..., Register Dst, Register Base,
                                            int64_t Offset) {
  if (isInt<12>(Offset)) {
    BuildMI(MBB, I, DL, TII.get(RVMin::ADDI), Dst).addReg(Base).addImm(Offset);
    return;
  }
  materializeConstant(MBB, I, TII, DL, Dst, Offset);   // Dst = Offset
  BuildMI(MBB, I, DL, TII.get(RVMin::ADD), Dst).addReg(Dst).addReg(Base);
}
```

> `Flag` 很重要：序言里的 `LUI/ADDI/SUB` 必须带 `MachineInstr::FrameSetup`，
> 收尾带 `FrameDestroy`，否则 PEI 的 `insertPrologEpilogCode` 和调试信息会
> 把这几条指令当成普通函数体指令。帧索引消除插入的指令在函数体里，用
> `NoFlags`（默认值）。

### 2. `eliminateFrameIndex`：从“拒绝”到“先算地址”

改动集中在一个 if：

```cpp
// RVMinRegisterInfo.cpp
if (isInt<12>(Offset)) {
  MI.getOperand(FIOperandNum).ChangeToRegister(FrameReg, /*isDef=*/false);
  MI.getOperand(FIOperandNum + 1).ChangeToImmediate(Offset);
  return false;
}

assert(FrameReg == RVMin::X2 && "RVMin only addresses frames through sp");

Register Scratch = MF.getRegInfo().createVirtualRegister(&RVMin::GPRRegClass);
RVMinFrameLowering::materializeAddress(MBB, II, TII, MI.getDebugLoc(), Scratch,
                                       FrameReg, Offset);

MI.getOperand(FIOperandNum).ChangeToRegister(Scratch, /*isDef=*/false);
MI.getOperand(FIOperandNum + 1).ChangeToImmediate(0);
```

于是

```text
sb a0, FI, 3000          ->   lui  a1, 1
                              addi a1, a1, -1080
                              add  a1, a1, sp
                              sb   a0, 0(a1)
```

这里 `-1080` 不是笔误：3 KiB 数组在帧里位于 `16(sp)`（16 字节是 emergency
slot 与对齐造成的位置），下标 3000 的地址是 `3000 + 16 = 3016`，而
`3016 = 4096 - 1080`。

### 3. 为什么用虚拟寄存器而不是直接借物理寄存器

`eliminateFrameIndex` 在 PEI 里运行，此时寄存器分配已经结束，**每个 GPR 都
可能正被占用**。这时有两个选择：

| 选择 | 结果 |
| --- | --- |
| 硬编码 `t0`（`X5`） | 大帧函数里 `t0` 可能正存着一个活值，直接破坏它 |
| 建一个虚拟寄存器 | 由一个后置 pass 统一分配；实在没有空寄存器就腾一个 |

上游 RISC-V 选后者，本章也一样。虚拟寄存器出现在 `eliminateFrameIndex` 里
看起来很奇怪，但 LLVM 专门为这件事准备了一条通道：

1. `RVMinRegisterInfo::requiresFrameIndexScavenging(MF)` 返回 `true`；
2. PEI 把 `FrameIndexVirtualScavenging` 置为 true，并在帧索引全部消除之后调用
   `scavengeFrameVirtualRegs(MF, *RS)`；
3. 这个 pass 只处理“帧索引消除留下的、还没有物理寄存器的 vreg”，
   实现方式是 `RegScavenger::scavengeRegisterBackwards(GPR, II, ...)`。

`requiresRegisterScavenging(MF)` 也必须返回 `true`，因为 PEI 只有在它为真时
才创建 `RegScavenger` 对象：

```cpp
// PEI::run
RS = TRI->requiresRegisterScavenging(MF) ? new RegScavenger() : nullptr;
FrameIndexVirtualScavenging = TRI->requiresFrameIndexScavenging(MF);
```

> 注意这两个开关是**分开**的。`requiresFrameIndexScavenging` 决定“帧索引消除
> 可以留 vreg”，`requiresRegisterScavenging` 决定“给我一个 scavenger”。
> 只开一个都不行。

### 4. `RS` 参数为什么在本章还是“没用”

`eliminateFrameIndex` 的签名里有 `RegScavenger *RS`，第 6 章起一直没碰它。
我们的实现仍然不碰，原因在 PEI 的两个循环：

```cpp
FrameIndexEliminationScavenging =
    (RS && !FrameIndexVirtualScavenging) ||
    TRI->requiresFrameIndexReplacementScavenging(MF);
```

- 反向遍历（默认）：`LocalRS = FrameIndexEliminationScavenging ? RS : nullptr`，
  我们让 `FrameIndexVirtualScavenging` 为 true，所以这里传的是 `nullptr`；
- 正向遍历：直接传 `RS`，但我们不需要它。

如果哪天要“就地”scavenge（像 PPC 那样），就需要
`requiresFrameIndexReplacementScavenging` 返回 true，并在
`eliminateFrameIndex` 里自己调 `RS->scavengeRegisterBackwards(...)`。
两条路的取舍写在练习 2 里。

### 5. emergency spill slot

scavenger 找到的寄存器如果正被占用，唯一合法的做法是**溢出到栈上**。
这个槽位不能临时申请（帧布局已经定了），必须由目标在
`processFunctionBeforeFrameFinalized` 提前预留：

```cpp
void RVMinFrameLowering::processFunctionBeforeFrameFinalized(
    MachineFunction &MF, RegScavenger *RS) const {
  MachineFrameInfo &MFI = MF.getFrameInfo();
  uint64_t Estimate = MFI.estimateStackSize(MF);   // 不含本槽位
  if (alignTo(Estimate + 4, 16) <= 2047)
    return;                                        // 小帧不需要

  const TargetRegisterClass &RC = RVMin::GPRRegClass;
  const TargetRegisterInfo &TRI = *MF.getSubtarget().getRegisterInfo();
  int FI = MFI.CreateSpillStackObject(TRI.getSpillSize(RC),
                                      TRI.getSpillAlign(RC));
  RS->addScavengingFrameIndex(FI);
}
```

三个细节：

1. `CreateSpillStackObject` 要在 `calculateFrameObjectOffsets` **之前**调用，
   否则偏移已经算完，槽位没有位置；
2. PEI 会把 scavenging 槽位放在“离 `sp` 最近”的位置（`AdjustStackOffset`
   在最后执行，并且对齐后还会再“float”一次），所以 scavenger 自己的
   `sw/lw` 永远落在 12 位范围内——这一点很关键，否则会无限递归；
3. 阈值用 `estimateStackSize + 4` 再对齐到 16，是因为估计值不含这个槽位，
   而帧最终会按 16 字节对齐。第 6 章的 `big`（2048 字节数组）因此从
   `addi sp, sp, -2048` 变成 `lui+addi+sub`，帧变成 2064，`big` 的 CHECK 也
   随之更新。

### 6. 序言/收尾复用同一个 helper

`adjustStackPointer` 现在是：

```cpp
if (isInt<12>(Amount)) {
  BuildMI(MBB, I, DL, TII.get(RVMin::ADDI), RVMin::X2)
      .addReg(RVMin::X2).addImm(Amount).setMIFlag(Flag);
  return;
}
int64_t Value = Amount < 0 ? -Amount : Amount;
RVMinFrameLowering::materializeConstant(MBB, I, TII, DL, RVMin::X5, Value, Flag);
BuildMI(MBB, I, DL, TII.get(Amount > 0 ? RVMin::ADD : RVMin::SUB), RVMin::X2)
    .addReg(RVMin::X2).addReg(RVMin::X5).setMIFlag(Flag);
```

`t0` 在这里仍然是安全的：序言里参数只在 `a0-a7`，收尾里 callee-saved 已经
恢复完；而 `eliminateFrameIndex` 在函数体中间，不能假设 `t0` 空闲——这就是
第 3 节必须用 vreg 的原因。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B" llc clang llvm-mc llvm-objdump llvm-readelf llvm-objcopy FileCheck

# 1. 本章 lit 测试（含 -O0..-O3 与 llvm-mc 往返）
"$B/bin/llvm-lit" -s "$W/llvm/test/CodeGen/RVMin/large-frame.ll"

# 2. 上篇 + 下篇全部门禁
"$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"

# 3. 3 KiB 帧在 QEMU 里跑（-O0 与 -O2 各一次）
"$W/llvm/lib/Target/RVMin/scripts/rvmin-run.sh" "$B/bin"
```

看 MIR 的两个位置：

```bash
# 帧索引消除之前：操作数还是 %stack.0.buf 与 3000
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O0 \
  -stop-after=finalize-isel -o - "$W/llvm/test/CodeGen/RVMin/large-frame.ll" \
  | grep -n "stack\.\|SB \|LBU "

# 帧索引消除之后：FI 变成 LUI/ADDI/ADD + 0(reg)
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O0 \
  -print-after=prolog-epilog -o /dev/null \
  "$W/llvm/test/CodeGen/RVMin/large-frame.ll" 2>&1 | grep -n "fi#\|LUI\|ADDI\|ADD "
```

`-print-after=prolog-epilog` 的头部还会列出帧对象，emergency slot 的位置一目了然：

```text
Frame Objects:
  fi#0: size=3072, align=16, at location [SP-3072]
  fi#1: size=4, align=4, at location [SP-3088]      <- scavenging slot，紧贴 sp
```

## 预期输出

3 KiB 函数的汇编（`-O2`，`-O0` 到 `-O3` 输出相同）：

```asm
big_frame:
        lui     t0, 1
        addi    t0, t0, -1008
        sub     sp, sp, t0           # sp -= 3088
        .cfi_def_cfa_offset 3088
        addi    a0, zero, 7
        lui     a1, 1
        addi    a1, a1, -1080
        add     a1, a1, sp           # a1 = sp + 3016
        sb      a0, 0(a1)
        lui     a0, 1
        addi    a0, a0, -1080
        add     a0, a0, sp
        lbu     a0, 0(a0)
        lui     t0, 1
        addi    t0, t0, -1008
        add     sp, sp, t0
        .cfi_def_cfa_offset 0
        jalr    zero, 0(ra)
```

非叶函数还会看到 callee-saved 槽位的远访问：

```asm
big_frame_call:
        lui     t0, 1
        addi    t0, t0, -992
        sub     sp, sp, t0
        lui     a0, 1
        addi    a0, a0, -996
        add     a0, a0, sp
        sw      ra, 0(a0)            # ra 槽位在帧的另一端
        .cfi_def_cfa_offset 3104
        .cfi_offset ra, -4
```

门禁输出：

```console
$ llvm-lit -s llvm/test/CodeGen/RVMin
Total Discovered Tests: 15
  Passed: 15 (100.00%)

$ scripts/rvmin-verify-all.sh $B/bin
PASS 1/5: target registered
PASS 2/5: lit (CodeGen + MC)
PASS 3/5: encodings
PASS 4/5: e2e
PASS 5/5: 88 llc runs with -verify-machineinstrs
ALL GATES PASS (run rvmin-run.sh separately for the QEMU check)

$ scripts/rvmin-run.sh $B/bin
rv32i: rvmin: ok
rv32i+m: rvmin: ok
PASS: 3 KiB frames (and 9 nested ones) ran under qemu-riscv32-static at -O0 and -O2
PASS: linked 6656 bytes (rv32i) and 6592 bytes (rv32i+m), ...
```

## 常见坑

### 坑 1：以为 `requiresFrameIndexScavenging` 一个开关就够了

只开 `requiresFrameIndexScavenging`，PEI 不会创建 `RegScavenger`，于是
`scavengeFrameVirtualRegs` 拿到空指针。反过来只开
`requiresRegisterScavenging`，`eliminateFrameIndex` 里建出来的 vreg 没人分配，
`-verify-machineinstrs` 会报“virtual register used after regalloc”。

### 坑 2：emergency slot 的时机与位置

- 在 `processFunctionBeforeFrameFinalized` 里创建，不能在 `emitPrologue` 里；
- 只给“可能用到”的帧加，否则每个 prologue 都多 4 字节；
- 阈值要包含 `+4` 和对齐，因为估计值不含槽位本身。写死 `estimate > 2047`
  会在 2044~2047 的帧上少留槽位：帧被撑到 2048 后，最靠上的对象正好越界。

### 坑 3：旧测试不是“随便挂的”

`stack.ll` 的 `big` 就是边界案例：2048 字节数组，偏移本身都能编码，但加上
emergency slot 后帧是 2064，数组挪到 `16(sp)`，prologue 也从 `addi` 变成
`LUI+ADDI`。这不是回归，而是“大于等于 2 KiB 的帧要预留 scavenging 槽位”的
必然结果；本章顺手把这个历史期望更新掉，并在测试里写了原因。

### 坑 4：`-O0` 才是压力测试

`-O2` 下值少、寄存器压力低，scavenger 几乎总能直接找到空闲寄存器；
`-O0` 的 fast regalloc 会留下更多活值。`rvmin-verify-all.sh` 第 5 项和
`large-frame.c` 的 `-O0` 变体就是为这个准备的。

### 坑 5：CFI 也会看到大数字

帧变成 3088 后，`.cfi_def_cfa_offset 3088` 是一条合法指令，但
`llvm-readelf --unwind` 的解析要能跟上；`llvm-mc -triple=rvmin` 往返测试
（`large-frame.ll` 的第 5 条 RUN）会同时覆盖汇编器和编码器。

## 练习

1. 把 `%p = getelementptr ... 3000` 改成 3060，看偏移怎么变；再改成 0，
   确认远访问消失但 prologue 仍然是 `LUI+ADDI`。
2. 试着把本章改成“就地 scavenge”：让
   `requiresFrameIndexReplacementScavenging` 返回 true，在
   `eliminateFrameIndex` 里直接调
   `RS->scavengeRegisterBackwards(RVMin::GPRRegClass, II, false, 0)`，
   然后比较两种实现的 MIR（`-print-after=prolog-epilog`）与代码尺寸。
3. 让 `materializeAddress` 先 `ADD Base` 再 `ADDI Lo`，验证结果相同，
   并解释为什么上游仍然选择“先加常量”。
4. 写一个 `-O0` 的函数，让 28 个 GPR 在远访问点同时活跃，观察
   `scavengeFrameVirtualRegs` 是否真的使用 emergency slot
   （提示：临时把 `isInt<12>` 的阈值改小更容易触发）。

## 下一章

第 17 章处理控制流：`RVMinISelLowering.cpp` 里 `CLI.IsTailCall = false` 会让
`return f(x);` 变成“调用 + 返回”。尾调用要复用入参寄存器与栈参数区、恢复
callee-saved 与 `sp`，并在 `j` 之前把 CFI 收到“已回到调用者帧”的状态。
