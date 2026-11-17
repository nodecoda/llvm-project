# 第 46 章：Zicsr 与 Zifencei：让 `rv32g` 合法

## 目标

第 43–45 章把压缩指令做成了完整的一篇，但对外仍然有一个刺眼的限制：
`clang --target=rvmin -march=rv32g` / `rv32gc` 会被 driver 直接拒绝。
原因不是 C，而是 `g` 的定义：

> `G = IMAFD_Zicsr_Zifencei`

第 43 章只差最后两块拼图，所以显式拒绝 `g`，避免“声称支持通用 ISA 却没有
CSR 指令”。本章补上这两块：

1. **Zicsr**：6 条 CSR 指令（`csrrw/csrrs/csrrc/csrrwi/csrrsi/csrrci`）
   + 7 条标准伪指令（`csrr/csrw/csrs/csrc/csrwi/csrsi/csrci`）；
2. **CSR 操作数文法**：汇编里既可以写名字（`mstatus`、`fflags`、`vl`），
   也可以写 12 位数字；打印器把数字还原成名字；
3. **Zifencei**：把 `fence.i` 从 `A` 的特性谓词迁移到 `Zifencei`；
4. **clang 侧解锁**：删除 driver 的 `g` 拒绝，`+zicsr/+zifencei` 进入
   target-features 白名单；
5. **验收**：24 条新编码与 `riscv32` 逐字节一致；反汇编往返；clang 矩阵
   从 6 个 ISA 扩到 8 个（`+rv32g/+rv32gc`），共 **384 个对象**。

本章**不改变代码生成**。没有任何 TableGen pattern 使用 CSR 指令，也没有新增
IR 内建；`hasSideEffects = 1` 只是保证 MC 层不会把它们当成可删除的纯计算。

## 背景：为什么 `g` 要等两章

`g` 不是一条扩展，而是一个缩写：

| 字母 | 展开 |
| --- | --- |
| `i` | 基础整数 |
| `m` | 乘除 |
| `a` | 原子（第 22 章） |
| `f` | 单精度浮点（第 40 章） |
| `d` | 双精度浮点（第 41 章） |
| `zicsr` | CSR 读写指令 |
| `zifencei` | `fence.i` |

在 43–45 章，前五项都齐了，只缺最后两项；而 `g` 一旦被接受，用户就会合理地
认为 `csrr a0, mstatus` 能汇编。所以在补齐之前，driver 宁可明确报错。本章的
第一件事就是把这条诊断删掉（`clang/lib/Driver/ToolChains/Arch/RISCV.cpp`）。

另一个历史遗留问题是 `fence.i`。第 22 章把它和 `fence` 一起放在
`Predicates = [HasStdExtA]` 下，这在教学上方便，但不符合规范：`fence.i` 属于
Zifencei，和原子指令无关。本章把它迁到独立谓词，于是：

- `-mattr=+a` 不再接受 `fence.i`；
- `-mattr=+zifencei` 在没有 A 时也能接受它；
- `rv32i` + `zifencei` 是合法组合。

代价是 A 语料的条数从 20 变成 19（`fence` 仍在 A 下，`fence.i` 移走），
编码门禁和反汇编门禁的计数都随之更新。

## 代码地图

| 位置 | 职责 |
| --- | --- |
| `RVMin.td` | `FeatureStdExtZicsr`、`FeatureStdExtZifencei`、两个 `generic-rv32-*` CPU |
| `RVMinSubtarget.h` | `HasStdExtZicsr`/`HasStdExtZifencei` 与查询接口 |
| `RVMinInstrInfo.td` | `csr_sysreg` 操作数、6 条指令、7 条 `InstAlias`、`FENCE_I` 迁移 |
| `MCTargetDesc/RVMinMCTargetDesc.{h,cpp}` | CSR 名字表（11 个名字）与 `parseCSRName`/`getCSRName` |
| `MCTargetDesc/RVMinInstPrinter.{h,cpp}` | `printCSRSystemRegister`、`printAliasInstr` 钩子 |
| `AsmParser/RVMinAsmParser.cpp` | `parseCSRSystemRegister`、`SystemReg` 操作数、`MatchOperandParserImpl` |
| `clang/lib/Driver/ToolChains/Arch/RISCV.cpp` | 删除 `g` 拒绝，保留 `+zicsr/+zifencei` |
| `clang/lib/Basic/Targets/RVMin.cpp` | 白名单、CPU 列表、target-features 过滤 |
| `scripts/rvmin-zicsr-encoding-corpus.s` | 24 条编码语料（23 CSR + `fence.i`） |
| `test/MC/RVMin/zicsr.s` | 编码、别名、数字 CSR、诊断、CPU 特性 |
| `scripts/check-rvmin-{encodings,disasm}.sh` | 新增 2k 段与 `roundtrip zicsr` |

## 关键实现

### 1. 6 条指令：I 型 + SYSTEM

CSR 指令的编码是 I 型，opcode 固定 `0b1110011`（SYSTEM）：

| funct3 | 指令 | rs1 的含义 |
| --- | --- | --- |
| `001` | `csrrw` | GPR（源值） |
| `010` | `csrrs` | GPR（置位掩码） |
| `011` | `csrrc` | GPR（清位掩码） |
| `101` | `csrrwi` | 5 位无符号立即数 `zimm` |
| `110` | `csrrsi` | `zimm` |
| `111` | `csrrci` | `zimm` |

TableGen 里用两个基类区分“rs1 是 GPR”和“rs1 是 zimm”，其余字段完全相同。
六个 `def` 都带 `hasSideEffects = 1`：即使 `rd = x0`（只写 CSR），也不能被当作
无副作用指令删除或重排。

### 2. CSR 操作数：名字是一个文法

CSR 在机器码里只是 12 位；名字只存在于汇编文本。实现分成三块：

1. `AsmOperandClass CSRSystemReg` 声明 `ParserMethod = "parseCSRSystemRegister"`，
   这样生成的 matcher 会在解析操作数时回调目标代码；
2. `parseCSRSystemRegister` 接受两种输入：
   - 标识符 → 查 `rvmin::parseCSRName`，未知名字报
     `unknown CSR name '...'`；
   - 表达式 → 必须折叠成 `0..4095`，否则报
     `CSR number must be an integer in the range 0..4095`；
3. 解析结果存成 `RVMinOperand::SystemReg`，`addCSRSystemRegisterOperands`
   把它变成 `MCOperand::createImm(Encoding)`。

打印器走 `printCSRSystemRegister`：查到名字就打印名字，查不到就打印数字。
CSR 名字表只有 11 项——课程实际会用到的那些：

```text
fflags 0x001   frm 0x002   fcsr 0x003
vstart 0x008   vl 0xc20    vtype 0xc21   vlenb 0xc22
mstatus 0x300  mtvec 0x305 mepc 0x341    mcause 0x342
```

这是一处**有意简化**：参考汇编器会按 F/V 特性检查 `fflags`/`vl` 是否可用，
RVMin 只检查“指令需要 Zicsr”，不检查“这个 CSR 需要哪个扩展”。第 47 章起 V
的 CSR 已经在表里，48–50 章写向量代码时可以直接用。

### 3. 7 条别名：`InstAlias` 而不是新 opcode

`csrr rd, csr` 就是 `csrrs rd, csr, x0`；`csrw csr, rs` 就是
`csrrw x0, csr, rs`。用 `InstAlias` 表达有三个好处：

- 汇编器接受短助记符，但永远生成规范指令的 `MCInst`，编码器不需要知道别名；
- 生成的反汇编器没有额外冲突（别名不进入 decoder 表）；
- asm writer 的别名表可以把规范指令打印回短形式。

别名打印需要三件配套：

1. `RVMinInstPrinter::printInst` 先调用 `printAliasInstr`；
2. `RVMinInstPrinter.cpp` 在 include 生成文件前 `#define PRINT_ALIAS_INSTR`；
3. 头文件声明 `printCustomAliasOperand`（生成代码会调用它）。

还有一个 TableGen 细节：`csrwi`、`csrw $csr, $imm` 和
`csrrw $rd, $csr, $imm` 在 `rd = x0` 时是同一个 `MCInst`。AsmWriterEmitter 会把
“条件和结果都相同”的别名判为 ambiguous 并**全部**丢掉，最后一条都不剩。
解决办法是给 `csrw/csrs/csrc $csr, $imm` 三个别名加 `EmitPriority = 0`：
汇编器仍然接受它们，但打印器只保留 `csrwi/csrsi/csrci`。

### 4. `fence.i` 迁移

```tablegen
let Predicates = [HasStdExtA] in {
def FENCE : RVMinFixedInst<0x0ff0000f, "fence">;
...
}

let Predicates = [HasStdExtZifencei] in
def FENCE_I : RVMinFixedInst<0x0000100f, "fence.i">;
```

`fence` 本身其实属于基础 I，但把它留在 A 下不影响本章目标；`fence.i` 必须
移走，否则 `+zifencei` 单独存在时反而不能汇编 `fence.i`。

### 5. clang：把 `g` 从“拒绝”改成“逐项展开”

driver 原来的逻辑是：

```cpp
if (StringRef(MArch).contains('g')) { reject; return; }
```

删除后，`rv32g` 走正常的 `RISCVISAInfo::parseArchString`，展开成
`+m,+a,+f,+d,+zicsr,+zifencei`。还需要两处白名单同步：

- driver 的 `erase_if` 保留 `+zicsr/+zifencei`；
- `RVMinTargetInfo::handleTargetFeatures` 的过滤同样保留它们，
  否则 target-features 属性会丢掉这两个特性，后端在 `llc` 路径上看不到。

ABI 由 `g` 里的 `d` 决定：`rv32g`/`rv32gc` 默认 `ilp32d`。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
ninja -C "$B" llc clang llvm-mc llvm-objdump llvm-readelf llvm-objcopy FileCheck
```

编码门禁：

```text
PASS: 85 RV32I + 12 M + 19 Zbb + 8 Zbs + 19 A + 28 F + 28 D + 35 C + 24 Zicsr/Zifencei instructions, .text identical
```

反汇编门禁（新增 `zicsr 24`，A 从 20 变 19）：

```text
PASS: a (19 instructions, .text round-trips byte-for-byte)
PASS: zicsr (24 instructions, .text round-trips byte-for-byte)
PASS: 172 encoding-corpus instructions + 35 compressed + 6 mixed + 24 zicsr/zifencei + 6 label-resolved branches/jumps
```

lit（31 个测试，含新的 `zicsr.s`）：

```text
Total Discovered Tests: 31
  Passed: 31 (100.00%)
```

clang 矩阵（8 个 ISA × 4 个优化级 × 12 个程序）：

```text
PASS: e2e corpus under -march=rv32g -O3
PASS: e2e corpus under -march=rv32gc -O3
ALL CLANG MATRIX PASS: 384 objects, ELF32 EM_RISCV, -S/-c agree, .text matches the riscv32 TargetInfo baseline
```

八项门禁：

```text
PASS 3/8: encodings
PASS 4/8: disassembler
PASS 5/8: e2e
PASS 6/8: 204 llc runs with -verify-machineinstrs
PASS 7/8: TableGen DSL contracts + status matrix + field effect
PASS 8/8: QEMU run (RV32I/M/A + F/D float/double + compressed demo)
ALL 8 GATES PASS
```

一个可复现的小实验：

```console
$ printf 'fence.i\ncsrr a0, mstatus\n' > /tmp/i.s
$ clang --target=rvmin-unknown-elf -march=rv32gc -mabi=ilp32d -c /tmp/i.s -o /tmp/i.o
$ llvm-objdump -d --triple=rvmin --mattr=+zicsr,+zifencei /tmp/i.o
       0: 0f 10 00 00  	fence.i
       4: 73 25 00 30  	csrr	a0, mstatus
```

## 验收表

| ID | 标准 | 证据 | 结果 |
| --- | --- | --- | --- |
| Z-AC1 | 6 条 CSR 指令的编码与 `riscv32 +zicsr` 逐字节相同 | 编码门禁 2k 段 | 通过 |
| Z-AC2 | 7 条别名汇编成规范指令且打印回别名 | `zicsr.s` 的 CHECK/DIS + `roundtrip zicsr` | 通过 |
| Z-AC3 | 名字与数字 CSR 产生相同编码 | `csrr a0, 0x300` ≡ `mstatus` | 通过 |
| Z-AC4 | 未知名/越界数字给出清晰诊断 | `zicsr.s` 的 BADCSR/RANGE | 通过 |
| Z-AC5 | 缺 `+zicsr`/`+zifencei` 分别报对应特性 | `zicsr.s` 的 MISSINGZICSR/MISSINGZIFENCEI | 通过 |
| Z-AC6 | `fence.i` 不再由 A 放行 | `-mattr=+a` 被拒；A 语料 20→19 | 通过 |
| Z-AC7 | `rv32g`/`rv32gc` 可编译，ABI 为 ilp32d | clang 矩阵 +384 对象 | 通过 |
| Z-AC8 | 不回归：8/8 门禁、31 lit、384 矩阵 | 本章实测输出 | 通过 |

## 常见坑

1. **别名打印不是自动的。** 只写 `InstAlias` 只会让汇编器接受短形式；
   要打印回去，必须调用 `printAliasInstr`、定义 `PRINT_ALIAS_INSTR`，
   并声明 `printCustomAliasOperand`。少一个就是“能汇编、打印仍是长形式”。
2. **相同结果的别名会被整组丢弃。** AsmWriterEmitter 的
   “Remove any ambiguous alias rules” 会把条件和结果都相同的别名全部删掉。
   给不需要打印的那几个设 `EmitPriority = 0`，而不是改条件去“骗”它。
3. **`MatchOperandParserImpl` 必须放在 `parseOperand` 最前面。** 它在
   `parseRegister`/`parseExpression` 之前给 CSV 名字一次机会；放后面的话
   `mstatus` 会先被解析成符号表达式。
4. **`fence.i` 不属于 A。** 迁移后 `-mattr=+a` 的单测会失败；A 语料、
   `roundtrip a` 的计数都要同步减一（20→19）。
5. **CSR 名字表要保守。** 参考汇编器会按 F/V 特性检查名字；RVMin 只收
   11 个课程用到的名字，其余走数字。不要因为“名字表越大越好”把特权 CSR
   全抄进来——教程只需要能解释的那部分。
6. **`hasSideEffects` 不是可选项。** `csrrs x0, mstatus, x0` 看起来什么
   都没改，但它可能读一个有副作用的 CSR；漏掉这个标记会在 MC 层之上引入
   难以复现的删除/重排。
7. **`g` 的 ABI 是 `ilp32d`。** `g` 包含 `d`，所以默认 ABI 跟着变成
   `ilp32d`；显式写 `-mabi=ilp32` 会被 TargetInfo 的 ABI 校验拒绝。
8. **TableGen 改动后要重跑 `dsl-surface.py`。** 本章新增了操作数类和指令，
   `--check` 会报告 “GENERATED TD USAGE is out of date”；用 `--update`
   重新生成第 29–39 章的契约块。

## 练习

1. 给 CSR 名字表加 `mscratch`/`mie`/`mip`/`mtval`，并验证
   `llvm-mc -triple=rvmin -show-encoding` 与 `riscv32` 仍然逐字节一致。
2. 实现 `csrrw` 的 `rd = x0` 与 `rs1 = x0` 的语义检查，讨论为什么
   “写 x0”在规范里是合法的（只写 CSR、不读旧值）。
3. 给 Zicsr 增加一个 `llvm.read_register`/`write_register` 的 IR 路径
   （提示：intrinsic + `SelectionDAG` 自定义节点），并解释为什么这已经
   超出“MC 层章节”的范围。
4. 用 `-march=rv32gc` 编译一个含 `__builtin_riscv_*` 的程序（如果当前
   clang 没有对应内建，就先写汇编），观察 CSR 指令在 QEMU 用户态的行为。
5. 比较 `csrr`、`csrrs rd, csr, x0` 和 `csrrs rd, csr, zero` 三种写法的
   编码与打印结果，解释别名表为什么最终只保留最短的那个。

## 产物清单

- 新增：`docs/46-zicsr-zifencei.md`、`test/MC/RVMin/zicsr.s`、
  `scripts/rvmin-zicsr-encoding-corpus.s`。
- 修改：`RVMin.td`、`RVMinSubtarget.h`、`RVMinInstrInfo.td`、
  `MCTargetDesc/*`、`AsmParser/RVMinAsmParser.cpp`、
  `clang/lib/Driver/ToolChains/Arch/RISCV.cpp`、
  `clang/lib/Basic/Targets/RVMin.cpp`、两个门禁脚本、clang 矩阵、
  `test/MC/RVMin/a-extension.s`、`scripts/rvmin-a-encoding-corpus.s`。
- tag：`rvmin-ch46`。
