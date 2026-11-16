# 第 17 章：尾调用与 musttail

## 目标

第 8 章的调用约定只会“先调用、再返回”。本章处理 C 里最常见的形状：

```c
int f(int x) { return g(x); }      /* 调用完立刻返回 */
```

完成本章后：

1. 尾位置的直接调用变成一条 `jalr zero, 0(t0)`（真正的 `j`），不再压自己的返回地址；
2. 尾位置的间接调用变成单条 `jalr zero, 0(rs)`；
3. 有栈帧、有 callee-saved 的函数在跳转**之前**先把 `sp` 与寄存器恢复好，
   `.eh_frame` 也在同一位置收回到调用者帧；
4. 参数放不进寄存器的尾调用安全回退成“普通调用 + 返回”；
5. `musttail` 无法满足时给出诊断，而不是悄悄退化；
6. 一百万层互递归在 QEMU 的默认 8 MiB 栈里跑通（没有 TCO 会 SIGSEGV）。

## 背景：`tail` 标记从哪里来

“尾调用”在 LLVM 里是两件事：

| 层次 | 谁负责 | 结果 |
| --- | --- | --- |
| IR | `-O2` 的 `TailCallElim` 给候选调用加 `tail` 标记 | `%r = tail call i32 @g(i32 %x)` |
| IR → DAG | `SelectionDAGBuilder::isInTailCallPosition` 判断调用是否**真的**在尾位置 | 不是尾位置就丢掉标记 |
| 后端 | `TargetLowering::LowerCall` 看 `CLI.IsTailCall` | 发 `CALL` 或 `TAIL` 节点 |

`tail` 只是“允许”，不是“保证”。例如：

```llvm
%call = tail call i32 @g(i32 %x)
%add  = add nsw i32 %call, 1     ; 结果还要用，不在尾位置
ret i32 %add
```

`isInTailCallPosition` 会拒掉它，后端仍然发普通调用。真正要求“必须尾调用”的是
`musttail`：

```llvm
%r = musttail call i32 @g(i32 %x)
ret i32 %r
```

IR 校验器对 `musttail` 有额外约束（返回类型、参数个数必须匹配），后端如果不支持
就必须报错——本章最后一节处理它。

## 代码地图

| 文件 | 本章改动 |
| --- | --- |
| `RVMinISelLowering.h/.cpp` | `mayBeEmittedAsTailCall`；`LowerCall` 的资格检查与 `RVMinISD::TAIL`；`musttail` 诊断 |
| `RVMinInstrInfo.td` | `RVMinTail` SDNode；`PseudoTAIL` / `PseudoTAILIND` 与两条 pattern |
| `RVMinAsmPrinter.cpp` | `emitPseudoCall` 增加 `IsTail` 参数：`jalr` 的 `rd` 从 `ra` 换成 `x0` |
| `RVMinFrameLowering.cpp` | 无需改动：`emitEpilogue` 本来就作用在“return block”上 |
| `llvm/test/CodeGen/RVMin/tail-call.ll` | 直接/间接/带帧/混合/musttail 回退 |
| `llvm/test/CodeGen/RVMin/tail-call-musttail-error.ll` | 栈参数 + `musttail` 的诊断 |
| `scripts/rt/tail.c`、`scripts/rvmin-run.sh` | 1e6 层尾递归的 QEMU 门禁 |

## 关键实现

### 1. 先判断“能不能真的复用帧”

尾调用的语义是：**把当前帧拆掉，再跳到被调用者**。RVMin 的调用约定是
`hasReservedCallFrame()`（出参区在固定帧里），因此拆帧之后 `sp` 正好回到
`entry_sp`，也就是普通调用放栈参数的位置。这条推理只在下面两个条件成立时有效：

```cpp
  bool IsTailCall = CLI.IsTailCall;
  if (IsTailCall &&
      (NumBytes != 0 || MF.getFrameInfo().hasVarSizedObjects())) {
    if (CLI.CB && CLI.CB->isMustTailCall())
      MF.getFunction().getContext().diagnose(DiagnosticInfoUnsupported{
          MF.getFunction(),
          "RVMin: musttail requires a fixed frame and arguments that fit in "
          "registers"});
    IsTailCall = false;
  }
  CLI.IsTailCall = IsTailCall;
```

- `NumBytes != 0`：有栈参数。普通调用可以先把参数写到自己的出参区再跳；尾调用
  没有“自己的出参区”了（`sp` 已经弹回去），所以必须回退。
- 动态对象（VLA）：帧大小不固定，`sp` 弹不回去。RVMin 目前本来也不支持。

注意 `CLI.IsTailCall` 要一起改：`TargetLowering::LowerCallTo` 用它断言
“尾调用不返回结果”（`InVals` 必须为空），只改局部变量会在 assert 构建里炸。

### 2. 没有 `CALLSEQ` 的调用

普通调用是：

```text
CALLSEQ_START(n)    -> ADJCALLSTACKDOWN（PEI 删掉）
  存栈参数 / 拷参数寄存器
  CALL              -> PseudoCALL / PseudoCALLR
CALLSEQ_END         -> ADJCALLSTACKUP（PEI 删掉）
  从 a0/a1 取结果
```

尾调用没有“回来”这回事，所以既不需要 `CALLSEQ`，也不需要取结果：

```cpp
  if (!IsTailCall)
    Chain = DAG.getCALLSEQ_START(Chain, NumBytes, 0, DL);
  ...
  if (IsTailCall) {
    MF.getFrameInfo().setHasTailCall();
    return DAG.getNode(RVMinISD::TAIL, DL, MVT::Other, Ops);
  }
```

`LowerCall` 仍然要返回一个合法 chain（`LowerCallTo` 会检查），只是它代表
“控制流到此为止”。

### 3. `isReturn = 1` 是全部关键

拆帧（`sp += N`）、恢复 callee-saved、发 CFI——这些代码第 6/8 章已经写好了，
它们在 `RVMinFrameLowering::emitEpilogue` 里，由 PEI 对每个 **return block** 调用。
所以尾调用伪指令只要声明成“返回”：

```tablegen
let isTerminator = 1, isBarrier = 1, isReturn = 1, Size = 12 in
def PseudoTAIL : RVMinPseudo<(outs), (ins bare_symbol:$func), "", []>;
```

PEI 的 `calculateSaveRestoreBlocks` 会把以它结尾的块当成退出块，于是：

```text
  addi sp, sp, 16          <- emitEpilogue
  .cfi_def_cfa_offset 0    <- emitPrologueCFI/emitEpilogue
  lui  t0, %hi(g)          <- PseudoTAIL 展开
  addi t0, t0, %lo(g)
  jalr zero, 0(t0)
```

`-print-after=prolog-epilog` 能直接看到这个顺序：

```text
bb.0.entry:
  $x2 = frame-setup ADDI $x2, -16
  ...
  $x2 = frame-destroy ADDI $x2, 16
  frame-destroy CFI_INSTRUCTION def_cfa_offset 0
  PseudoTAIL target-flags(<unknown>) @g, <regmask ...>, implicit $x10
```

反过来说：如果忘了 `isReturn`，`sp` 不会被恢复，程序会“跳进”被调用者但把调用者的
帧留在栈上——一百万层之后栈就爆了，而小测试可能完全看不出来。

### 4. 间接尾调用与地址物化

间接尾调用只有一条指令：

```tablegen
let isTerminator = 1, isBarrier = 1, isReturn = 1, Size = 4 in
def PseudoTAILIND : RVMinPseudo<(outs), (ins GPR:$rs1), "", []>,
                    PseudoInstExpansion<(JALR X0, GPR:$rs1, 0)>;
```

直接尾调用和 `PseudoCALL` 一样需要 `LUI+ADDI+JALR` 三段（JAL 只有 ±1 MiB），
只是最后一段的 `rd` 是 `x0`。所以 AsmPrinter 里把 `emitPseudoCall` 参数化即可：

```cpp
    Jalr.addOperand(MCOperand::createReg(IsTail ? RVMin::X0 : RVMin::X1));
```

`rd = x0` 意味着**不写 `ra`**：被调用者返回时用的是我们调用者的返回地址，
这正是尾调用省掉一层栈帧的原因。

### 5. `musttail` 的诊断

`tail` 是提示，回退是合法的；`musttail` 是契约，回退必须报错。用
`DiagnosticInfoUnsupported` 而不是 `report_fatal_error`，输出是普通的前端错误，
不会带崩溃栈：

```console
$ llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 must2.ll -o /dev/null
error: <unknown>:0:0: in function needs_stack i32 (i32, ...):
       RVMin: musttail requires a fixed frame and arguments that fit in registers
```

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B" llc clang llvm-mc llvm-objdump llvm-readelf llvm-objcopy FileCheck

# 1. 本章 lit（直接/间接/带帧/混合/回退 + 诊断）
"$B/bin/llvm-lit" -s "$W/llvm/test/CodeGen/RVMin/tail-call.ll" \
                     "$W/llvm/test/CodeGen/RVMin/tail-call-musttail-error.ll"

# 2. 全部门禁（lit / 编码 / e2e / -verify-machineinstrs）
"$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"

# 3. QEMU：3 KiB 帧 + 1e6 层尾递归
"$W/llvm/lib/Target/RVMin/scripts/rvmin-run.sh" "$B/bin"
```

观察中间状态：

```bash
# IR 里是否带 tail 标记（clang -O2 才有）
"$B/bin/clang" --target=riscv32-unknown-elf -march=rv32i -mabi=ilp32 -O2 \
  -ffreestanding -S -emit-llvm "$W/llvm/lib/Target/RVMin/scripts/rt/tail.c" -o - \
  | grep "tail call" | head

# PEI 之后的尾调用块：拆帧 → CFI → PseudoTAIL
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O2 \
  -print-after=prolog-epilog -o /dev/null \
  "$W/llvm/test/CodeGen/RVMin/tail-call.ll" 2>&1 \
  | sed -n '/After PrologEpilogInserterPass on frame_tail/,/End machine code/p'

# 机器验证器（每章必跑）
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O2 \
  -verify-machineinstrs -filetype=obj -o /dev/null \
  "$W/llvm/test/CodeGen/RVMin/tail-call.ll"

# unwind：尾调用函数的 FDE 必须以“CFA = sp、没有已保存寄存器”结尾
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O2 -filetype=obj \
  -o /tmp/tail.o "$W/llvm/test/CodeGen/RVMin/tail-call.ll"
"$B/bin/llvm-readelf" --unwind /tmp/tail.o | sed -n '/0x34/,/0x5c/p'
```

## 预期输出

直接尾调用：

```asm
direct:
        lui     t0, %hi(g)
        addi    t0, t0, %lo(g)
        jalr    zero, 0(t0)          # rd = x0：不压返回地址
```

间接尾调用（参数先搬进 a0，地址留在另一个寄存器）：

```asm
indirect:
        addi    a2, a0, 0
        addi    a0, a1, 0
        jalr    zero, 0(a2)
```

有帧的尾调用：先拆帧，再跳。

```asm
frame_tail:
        addi    sp, sp, -16
        .cfi_def_cfa_offset 16
        sw      a0, 12(sp)
        lw      a0, 12(sp)
        addi    sp, sp, 16
        .cfi_def_cfa_offset 0
        lui     t0, %hi(g)
        addi    t0, t0, %lo(g)
        jalr    zero, 0(t0)
```

混合（一个普通调用 + 一个尾调用）：`ra` 只保存一次。

```asm
mixed:
        addi    sp, sp, -16
        sw      ra, 12(sp)
        .cfi_def_cfa_offset 16
        .cfi_offset ra, -4
        lui     t0, %hi(h)
        addi    t0, t0, %lo(h)
        jalr    ra, 0(t0)
        lw      ra, 12(sp)
        addi    sp, sp, 16
        .cfi_restore ra
        .cfi_def_cfa_offset 0
        lui     t0, %hi(g)
        addi    t0, t0, %lo(g)
        jalr    zero, 0(t0)
```

`mixed` 的 FDE（`llvm-readelf --unwind`，节选）：

```text
  [0x54] FDE length=24
    initial_location: 0x34   address_range: 0x28 (end : 0x5c)
    Program:
      DW_CFA_advance_loc: 8 to 0x3c
      DW_CFA_def_cfa_offset: +16      <- 压栈
      DW_CFA_offset: reg1 -4          <- 保存 ra
      DW_CFA_advance_loc: 20 to 0x50
      DW_CFA_restore: reg1            <- 恢复 ra
      DW_CFA_def_cfa_offset: +0       <- sp 回到 caller 帧；之后才是尾跳转
```

门禁输出：

```console
$ llvm-lit -s llvm/test/CodeGen/RVMin llvm/test/MC/RVMin
Total Discovered Tests: 17
  Passed: 17 (100.00%)

$ scripts/rvmin-verify-all.sh $B/bin
PASS 1/5: target registered
PASS 2/5: lit (CodeGen + MC)
PASS 3/5: encodings
PASS 4/5: e2e
PASS 5/5: 96 llc runs with -verify-machineinstrs
ALL GATES PASS (run rvmin-run.sh separately for the QEMU check)

$ scripts/rvmin-run.sh $B/bin
...
PASS: 3 KiB frames (and 9 nested ones) ran under qemu-riscv32-static at -O0 and -O2
PASS: 1e6 tail calls ran under qemu-riscv32-static (16 MiB of frames would not fit)
```

最后一条门禁是有区分度的：把同一个 `tail.c` 用 `-O0` 编译（clang 不打 `tail`
标记）再跑，会直接 `Segmentation fault`：

```console
$ qemu-riscv32-static ./tail0 ; echo $?
Segmentation fault
139
```

## 常见坑

### 坑 1：忘了 `isReturn`

只加 `isTerminator/isBarrier` 的话，PEI 不认为这个块会退出，`sp` 和 callee-saved
都不会恢复。小函数（无帧、无 callee-saved）看起来仍然正确，直到递归足够深。

### 坑 2：`CALLSEQ_START` 没有条件化

尾调用不需要 `CALLSEQ_START`。如果照搬普通调用的代码，PEI 会在尾调用块里插入
一对 `ADJCALLSTACKDOWN/UP`（虽然最终被删掉），但更糟的是 `sp` 的调整会被算进
`SPAdj`，帧索引消除和 CFI 都会错。

### 坑 3：`CLI.IsTailCall` 与局部变量不一致

`LowerCallTo` 里有两个断言用 `CLI.IsTailCall` 判断“是否应该有返回值”：

```cpp
  assert((!CLI.IsTailCall || InVals.empty()) &&
         "LowerCall emitted a return value for a tail call!");
```

如果你在 `LowerCall` 里决定回退，但忘了写回 `CLI.IsTailCall = false`，assert
构建会在这里炸；release 构建则会把结果值丢掉。

### 坑 4：以为 `-O0` 也有尾调用

`tail` 标记来自 `TailCallElim`，它只在优化管线里跑。`-O0` 的 IR 没有标记，
所以 `-O0` 下 `return f(x)` 仍然是普通调用——这也是本章 QEMU 门禁只跑 `-O2` 的
原因。反过来，`musttail` 在 `-O0` 也必须生效（它在 IR 里就写死了）。

### 坑 5：CFI 顺序

`.cfi_restore` / `.cfi_def_cfa_offset 0` 必须在跳转**之前**、`sp` 调整**之后**。
`emitEpilogue` 用 `MBB.getFirstTerminator()` 作为插入点，所以 `PseudoTAIL` 必须是
块里最后一条指令；如果后面又插了调试指令或 CFI，顺序就会乱。

## 练习

1. 让 `RVMinISelLowering::LowerCall` 对“有栈参数”也发尾调用（提示：需要在跳转前把
   出参写进 `entry_sp + offset`，并把 `sp` 恢复到 `entry_sp` 之后再写）。对比
   RISC-V 上游 `isEligibleForTailCallOptimization` 的检查项。
2. 把 `PseudoTAIL` 的展开改成单条 `j`（`JAL x0, target`），量一下代码尺寸与
   ±1 MiB 范围限制的取舍。
3. 用 `llvm-objdump --triple=riscv32 -d` 反汇编 `tail.o`，确认 `jalr zero, 0(t0)`
   的 `rd` 字段是 0（提示：`0x00028067` 与 `0x000280e7` 的差别）。
4. 给 `tail-call.ll` 加一个“尾调用 + 返回值位宽不同”的用例（例如 `i8` 返回到
   `i32`），观察 `LowerReturn` 的扩展是否仍然正确。

## 下一章

第 18 章处理 `switch`：`RVMinISelLowering.cpp` 现在把 `BR_JT`/`BRIND` 设为
`Expand`，所以 `switch` 只能降级成比较链。下一章要实现跳转表（`.word LBB` 表项 +
间接跳转），让 8 分支的 `switch` 只要一次 load 加一次 `jalr`。
