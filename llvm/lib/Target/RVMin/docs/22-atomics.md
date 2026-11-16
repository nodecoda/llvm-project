# 第 22 章：A 扩展与 AtomicExpand

## 目标

前 21 章里，任何 `atomicrmw`、`cmpxchg` 或 `load atomic` 都会掉进
`__atomic_*` 库调用：RVMin 没有告诉 LLVM“我能原子地做 32 位操作”，
默认的 `MaxAtomicSize = 0` 让 `AtomicExpandPass` 把所有原子操作都换成 libcall。

本章打开 A 扩展，做完之后：

1. `-mattr=+a` 下 `atomicrmw add/and/or/xor/xchg/min/max` 各是一条 `amo*.w.aqrl`；
2. 没有对应 AMO 的 `atomicrmw sub/nand` 和 `cmpxchg` 变成 `lr.w.aqrl`/`sc.w.aqrl`
   重试循环；
3. `load atomic`/`store atomic` 是普通 `lw`/`sw` 加一条 `fence`；
4. `fence`、`fence.i` 与 11 条 A 指令的编码与参考汇编器逐字节相同；
5. 不开 A 时输出仍是第 21 章的 `__atomic_*` 调用，一个字都不变。

## 背景：原子性不是一个指令的事

RISC-V 把原子内存操作拆成两层：

| 指令 | 语义 |
| --- | --- |
| `lr.w rd, (rs1)` | 读出并登记一个保留（reservation） |
| `sc.w rd, rs2, (rs1)` | 若保留仍有效则写入，`rd = 0` 表示成功 |
| `amo<op>.w rd, rs2, (rs1)` | 一次完成读-改-写，返回旧值 |
| `fence` | 内存序屏障 |

`lr.w`/`sc.w` 需要软件写循环，`amo*` 不需要。因此一个真实后端要回答四个问题：

1. 目标最多支持多大的原子操作？（`setMaxAtomicSizeInBitsSupported`）
2. 原子 load/store 是自己发射，还是拆成 `fence + 普通访存`？
   （`shouldInsertFencesForAtomic`）
3. 哪些 `atomicrmw` 直接用 AMO，哪些要降级成 `cmpxchg` 循环？
   （`shouldExpandAtomicRMWInIR`）
4. `cmpxchg` 怎么变成 `lr/sc` 循环？（`shouldExpandAtomicCmpXchgInIR` +
   `emitLoadLinked`/`emitStoreConditional`）

本章的答案都取“保守但正确”：

- 每条 AMO 都用 `.aqrl` 编码。它比需要的强，但对任何 ordering 都合法；
- 原子 load/store 用 `fence` + 普通 `lw`/`sw`，因为 RV32 没有“带 ordering 的 load”；
- 因此 `seq_cst` 的 `atomicrmw` 会看到 `fence; amoadd.w.aqrl; fence` 三条指令。
  多出来的 fence 是教学取舍：先把语义做对，再在练习里删掉。

## 代码地图

| 位置 | 职责 |
| --- | --- |
| `RVMin.td` | `FeatureStdExtA`、`HasStdExtA`、`generic-rv32-a` |
| `RVMinSubtarget.h` | `HasStdExtA` 字段与 `hasStdExtA()` |
| `RVMinInstrFormats.td` | `OPC_AMO = 0b0101111`、`OPC_MISC_MEM = 0b0001111` |
| `RVMinInstrInfo.td`（A 段） | `RVMinAMOBase/LR/RO`、11 条指令、`FENCE`/`FENCE_I`、选择 pattern |
| `RVMinISelLowering.cpp` | `setMaxAtomicSizeInBitsSupported`、四个 atomic hook、inline asm |
| `RVMinISelLowering.h` | hook 声明与 `getRegForInlineAsmConstraint` |
| `RVMinCodeGenPassBuilder.cpp` | 在 ISel 之前插入 `AtomicExpandPass` |
| `RVMinAsmParser.cpp` | 接受裸 `(rs1)`（零位移访存） |
| `RVMinAsmPrinter.cpp` | inline asm 的寄存器操作数打印 |
| `scripts/rvmin-a-encoding-corpus.s` | 20 条编码语料 |
| `scripts/rt/atomics.c` | QEMU 运行门禁 |
| `llvm/test/MC/RVMin/a-extension.s` | 编码固定值 + 缺特性诊断 + 反汇编 |
| `llvm/test/CodeGen/RVMin/atomics.ll` | AMO/LL-SC/fence 的选择结果 |

## 关键实现

### 1. 特性三件套与编码

与第 13、21 章完全同构：`SubtargetFeature`、`Predicate`、`AssemblerPredicate`
共用 `HasStdExtA`。缺特性时汇编器必须报：

```text
error: instruction requires the following: 'A' (Atomic Instructions)
```

A 指令全部是 R 型布局，但字段含义与普通 R 型不同：

```text
 31..27   26 25   24..20   19..15   14..12   11..7   6..0
 funct5    aq rl   rs2      rs1      funct3   rd     opcode
                                                    0101111
```

- `funct3 = 010` 对全部 A 指令固定；
- `lr.w` 的 `rs2 = 0`；
- `sc.w` 的 `funct5 = 00011`，其余 AMO 用 `00001/00000/00100/01100/01000/
  10000/10100/11000/11100` 区分。

TableGen 里用两个类 + 两个 multiclass 生成四种拼写：

```tablegen
multiclass RVMinAMOLR<string base> {
  def ""    : RVMinAMOLR<0b00, base>;          // lr.w
  def _AQ   : RVMinAMOLR<0b10, base # ".aq">;  // lr.w.aq
  def _RL   : RVMinAMOLR<0b01, base # ".rl">;  // lr.w.rl
  def _AQRL : RVMinAMOLR<0b11, base # ".aqrl">;// lr.w.aqrl
}
```

### 2. 汇编器：裸 `(rs1)`

打印器输出 `lr.w a0, (a1)`，所以解析器必须接受没有偏移量的括号操作数。
原来的 `parseOperand` 只在表达式解析成功后才看 `(`；而 `(a1)` 会被当成
“带括号的符号表达式”吃掉。修法是先看 `(`，再用 `peekTok()` 确认后面是寄存器：

```cpp
if (getLexer().is(AsmToken::LParen)) {
  const AsmToken &Next = getLexer().peekTok();
  if (Next.is(AsmToken::Identifier) &&
      matchRegister(Next.getIdentifier()) != RVMin::NoRegister) {
    if (parseMemOpBaseReg(Operands).isFailure())
      return Error(getLoc(), "expected memory base register");
    return false;
  }
}
```

保留 `peekTok` 判断是有意的：`addi a0, a1, (1+2)` 仍然按表达式解析。

### 3. 把 AtomicExpand 接进管线

`AtomicExpandPass` 不是通用 codegen 管线的一部分，必须由目标自己加。
`RVMinCodeGenPassBuilder` 新增：

```cpp
void RVMinCodeGenPassBuilder::addIRPasses(PassManagerWrapper &PMW) {
  addFunctionPass(AtomicExpandPass(TM), PMW);
  Base::addIRPasses(PMW);
}
```

顺序很重要：原子操作必须在 ISel 之前就变成目标能选择的形式。

### 4. 四个 hook

```cpp
setMaxAtomicSizeInBitsSupported(STI.hasStdExtA() ? 32 : 0);
setMinCmpXchgSizeInBits(32);

bool shouldInsertFencesForAtomic(const Instruction *) const override {
  return true;
}
```

- `MaxAtomicSize = 0`（无 A）是“全部走 libcall”的开关；
- `MinCmpXchgSize = 32` 让 i8/i16 的原子操作先被 “partword” 扩宽成 32 位
  read-modify-write，再进入同一个 LL/SC 循环；
- `shouldInsertFencesForAtomic = true` 把 ordering 交给 `fence`，普通 `lw`/`sw`
  才有机会被选择。

`atomicrmw` 的分流：

```cpp
if (!RMW->getType()->isIntegerTy(32))
  return AtomicExpansionKind::CmpXChg;      // i8/i16 先扩宽
switch (RMW->getOperation()) {
case Add: case And: case Or: case Xor: case Xchg:
case Max: case Min: case UMax: case UMin:
  return AtomicExpansionKind::None;         // 有 AMO，留给 ISel
default:
  return AtomicExpansionKind::CmpXChg;      // sub/nand 等没有 AMO
}
```

`None` 的语义是“别在 IR 里展开”，于是 `ATOMIC_LOAD_ADD` 等节点活到
SelectionDAG，被下面的 pattern 吃掉：

```tablegen
class RVMinAMOPat<SDPatternOperator OpNode, RVMinInst Inst>
    : Pat<(i32 (OpNode (i32 GPR:$rs1), (i32 GPR:$rs2))),
          (Inst GPR:$rs2, GPR:$rs1)>;

def : RVMinAMOPat<atomic_load_add, AMOADD_W_AQRL>;
```

操作数顺序容易写反：AMO 的 `InOperandList` 是 `(rs2, rs1)`，即
“值在前、地址在后”，而 DAG 节点是 `(address, value)`。

### 5. cmpxchg 与 inline asm

`shouldExpandAtomicCmpXchgInIR` 返回 `LLSC`，`AtomicExpandPass` 就会调用
`emitLoadLinked`/`emitStoreConditional` 搭循环。RVMin 还没有 target intrinsic，
所以这两个 hook 直接发一条 inline asm：

```cpp
return emitAtomicAsm(Builder, "lr.w.aqrl $0, ($1)", ...);
return emitAtomicAsm(Builder, "sc.w.aqrl $0, $2, ($1)", ...);
```

配套要补两处：

- `getRegForInlineAsmConstraint`：把 `"r"` 映射到 `GPRRegClass`，否则
  寄存器分配报 `could not allocate output register for constraint 'r'`；
- `PrintAsmOperand`：把机器寄存器打印成 ABI 名，否则汇编器看到
  `lr.w.aqrl , ()`。

生成的重试循环（`-O2`，`cmpxchg`）：

```asm
	#APP
	lr.w.aqrl a0, (a3)
	#NO_APP
	bne a0, a1, .LBB3_5
	fence
.LBB3_2:
	#APP
	sc.w.aqrl a4, a2, (a3)
	#NO_APP
	beq a4, zero, .LBB3_4
	#APP
	lr.w.aqrl a0, (a3)
	#NO_APP
	beq a0, a1, .LBB3_2
```

注意 `sc.w` 的返回值含义：**0 表示成功**，所以循环条件是“返回值非 0 就重试”。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B" llc llvm-mc llvm-objdump llvm-lit FileCheck

"$B/bin/llvm-lit" -sv "$W/llvm/test/MC/RVMin/a-extension.s" \
                       "$W/llvm/test/CodeGen/RVMin/atomics.ll"
"$W/llvm/lib/Target/RVMin/scripts/check-rvmin-encodings.sh" "$B/bin"
"$W/llvm/lib/Target/RVMin/scripts/check-rvmin-disasm.sh" "$B/bin"
"$W/llvm/lib/Target/RVMin/scripts/rvmin-run.sh" "$B/bin"
```

## 预期输出

编码门禁多出第五组：

```text
PASS: 85 RV32I + 12 M + 19 Zbb + 8 Zbs + 20 A instructions, .text identical
```

反汇编门禁多出 A 的往返：

```text
PASS: a (20 instructions, .text round-trips byte-for-byte)
PASS: 117 encoding-corpus instructions + 6 label-resolved branches/jumps
```

选择结果：

```text
$ llc -march=rvmin -mattr=+a -O2 -o - atomics.ll | grep -E 'amo|lr\.w|sc\.w|fence'
	fence
	amoadd.w.aqrl a0, a1, (a0)
	fence
	lw a0, 0(a0)
	fence
```

运行门禁：

```text
atomics: rvmin: ok
PASS: A-extension atomics ran under qemu-riscv32-static (amoadd + lr.w/sc.w loops)
```

## 常见坑

1. **`setMaxAtomicSizeInBitsSupported` 在 `TargetLowering` 上，不在
   `TargetMachine` 上。** 写进 `RVMinTargetMachine` 构造函数会直接编译失败。
2. **`cmpxchg` 的 `AtomicExpansionKind::Expand` 是未处理的 case。**
   `tryExpandAtomicCmpXchg` 的 switch 只有 `None/LLSC/MaskedIntrinsic/NotAtomic/
   CustomExpand`，`Expand` 会 `llvm_unreachable`。要用 `LLSC`。
3. **`atomicrmw` 没有 AMO 时不能返回 `Expand`。** 应该返回 `CmpXChg`；
   `Expand` 是给“元素级”向量原子用的。
4. **inline asm 需要两处配套**：寄存器约束映射与操作数打印，缺一则分别在
   寄存器分配和 MC 汇编阶段失败。
5. **`sc.w` 成功返回 0。** 把 `0` 当失败会让循环永远重试。
6. **QEMU 6.2 的 ELF loader 会踩纯 `.bss` 段。** 如果 `p_filesz == 0` 的
   `PT_LOAD` 与 `.text` 共享一页，加载器会把整页清零，程序在第一条指令
   就 SIGILL。`scripts/rt/atomics.c` 给两个全局量加了非零初值（仍会在 `main`
   里重置），把段变成文件-backed，从而绕开这个 loader quirk。
7. **clang 用 `-march=rv32i` 编译 `__atomic_*` 会刷 `-Watomic-alignment` 警告。**
   运行门禁用 `-march=rv32ia` 表达真实意图；codegen 仍然由 `llc -mattr=+a` 决定。
8. **`llvm-objdump` 的 `-mattr` 要写 `--mattr=+a`**，否则会被当成 `-m attr=+a`。

## 练习

1. 把 AMO pattern 按 ordering 拆成 `.aq`/`.rl`/`.aqrl` 四套（照上游
   `AMOPat` 的写法），然后去掉 `fence; amoadd; fence` 里多余的两条 fence。
2. 给 `sub` 增加一条 `AMOADD.W` + `neg` 的选择，观察 `atomics.ll` 里
   LL/SC 循环是否消失。
3. 把 `shouldInsertFencesForAtomic` 改成“只对 load/store 返回 true”，
   验证 `cmpxchg` 的 fence 数量变化，并解释为什么仍然正确。
4. 用 `__atomic_fetch_max`/`__atomic_fetch_min` 扩展 `atomics.c`，让
   `amomax.w`/`amomin.w` 进入 QEMU 门禁；注意有符号与无符号的差别。
5. 写一个双线程版本（需要 `clone`），让 `sc.w` 真的失败一次，并用
   `--trace` 统计重试次数。思考：QEMU user mode 下如何制造竞争？
