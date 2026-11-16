# 第 09 章：M/Zbb/Zbs/A 与 `--isa`

## 目标

让模拟器不再只有 RV32I：把第一课实现过的 M、Zbb、Zbs、A 扩展搬进来，
并用一个**运行时可切换的 ISA 掩码**控制哪些指令可以执行：

- 新增 17 条 Zbb、8 条 Zbs、11 条 A 指令（共 36 条）；`insn.def` 总计 92 条
  （RV32I/Zicsr 48、M 8、Zbb 17、Zbs 8、A 11）；
- `--isa=rv32i[m][_zbb][_zbs][_a]`（也接受 `--isa <spec>`）缩小可执行集合；
- 被掩掉的指令必须走**非法指令 trap**，而不是“悄悄执行”；
- 用第一课的三份语料（19 + 8 + 20 条）与 `llvm-mc -disassemble` 逐行对拍。

本章结束时，同一个 `muldiv` 源码能在 `--isa=rv32i`（libcall）和
`--isa=rv32im`（真实乘法）下分别正确运行——ISA 掩码第一次有了可观察的行为。

## 背景：为什么是“掩码”而不是重新编译

第一课的 `-march=rvmin` 固定产出 RV32I+M 的编码；扩展指令的编码在
`insn.def` 里本来就有区分度（opcode/funct3/funct7），所以译码器不需要分支，
只需要一张“这条指令属于哪个扩展”的表：

```cpp
// insn.def 的 isa 列，X-macro 展开成 isaOf(id)
RSIM_INSN(ADD,  "add",  FmtR, 0x33, 0x0, 0x00, 0xff, RV32I, 0xff)
RSIM_INSN(MUL,  "mul",  FmtR, 0x33, 0x0, 0x01, 0xff, RV32M, 0xff)
RSIM_INSN(CLZ,  "clz",  FmtUnary, 0x13, 0x1, 0x30, 0x600, Zbb, 0xff)
```

执行前查一次 `isa_ & isaOf(id)`，比“编译期两份模拟器”更贴近真实硬件：
同一颗核，`misa` 决定软件能不能用某条指令。

三组扩展的定位：

| 扩展 | 内容 | 参考 |
| --- | --- | --- |
| M | 乘除、余数（8 条） | 第一课 ch19 已实现编码与语义 |
| Zbb | 位操作基础（`clz/ctz/cpop/rol/ror/min/max/...`，17 条） | 第一课 ch21 的编码语料 |
| Zbs | 单比特置位/清零/取反/测试（8 条） | 同上 |
| A | `lr.w/sc.w` + 9 条 AMO（11 条） | 第一课 ch22 的 A 语料 |

QEMU 6.2.0 支持 M 与 A，**不支持 Zbb/Zbs**，所以后两组只能与 `llvm-mc` 对拍；
M 与 A 还能额外用 QEMU 做行为差分。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `rsim/insn.def` | 93 条指令；新增第 9 列 `match_f5`（bits 31:27） |
| `rsim/decode.h` | `Isa{RV32I,RV32M,Zbb,Zbs,A}`、`Fmt::Unary/Atomic` |
| `rsim/decode.cpp` | `match_f5` 过滤、Unary/Atomic 的 `format()` |
| `rsim/cpu.cpp` | 36 条指令的语义 + `isaOf(id)` 掩码检查 |
| `rsim/main.cpp` | `parseIsa()`、`--isa=`/`--isa` 两种写法 |
| `rsim/selftest.cpp` | `encA()` 编码器 + Zbb/Zbs/A 向量 |
| `scripts/course2-verify.sh` | 三份语料的 `decode_corpus` + 严格模式检查 |

## 关键实现

### 1. `match_f5`：A 扩展的第五个 funct 字段

RISC-V 的 A 指令把 `funct5` 放在 bits 31:27，而 `aq/rl`（bit 26/25）只是
内存序提示。第 9 列 `match_f5` 让我们能：

```text
lr.w     funct5=0x02   sc.w   funct5=0x03
amoswap  0x01  amoadd 0x00  amoxor 0x04  amoand 0x0c  amoor 0x08
amomin   0x10  amomax 0x14  amominu 0x18  amomaxu 0x1c
```

**aq/rl 被故意忽略**：单 hart 模拟器没有任何东西需要排序，`lr.w.aqrl`
与 `lr.w` 对我们是同一条指令。因此译码器把它们折叠成同一个助记符，
门禁对 reference 文本做 `sed -E 's/\.(aqrl|aq|rl) / /'` 归一化后再 diff。
这是本章唯一一处“与 llvm-mc 文本不同但语义正确”的地方，文档必须写明。

### 2. Zbb 的一元指令没有 `rs2`

`clz/ctz/cpop/sext.b/sext.h/rev8/zext.h` 的编码借用 I 型立即数字段
（bits 31:20）做区分，没有第二个源寄存器。为此新增 `Fmt::Unary`：
`format()` 只打印 `rd, rs1`，执行器也从 `d.rs1` 取值。

`rol/ror/min/minu/max/maxu/andn/orn/xnor` 则是标准 R 型，
只是 `funct7` 与 RV32I 不同。

### 3. Zbs 的两套编码

`bset/bclr/binv/bext` 是 R 型（`rs2` 低 5 位是位号）；
`bseti/bclri/binvi/bexti` 是 I 型移位格式（位号在 `shamt`，bits 24:20）。
两者复用第 03 章的 `FmtR`/`FmtShift`，执行器只差一个取操作数的位置。

### 4. `isaOf()` 与 `parseIsa()`

`insn.def` 的 isa 列展开成 `isaOf(Insn)`；`Cpu::step()` 在执行前检查：

```cpp
if ((isa_ & static_cast<uint32_t>(isaOf(d.id))) == 0)
  throw Trap(TrapCause::IllegalInstruction, word, at,
             "instruction not enabled by --isa");
```

`parseIsa()` 接受 `rv32i`、`rv32im`、`rv32i_zbb_zbs_a`、`rv32g` 等写法，
缺省是 `RV32I|RV32M`（与第一课默认产物一致），所以 Zbb/Zbs/A 必须显式写进
`--isa` 才能执行。`zicsr` 总是允许，因为第 08 章的 CSR 指令没有单独开关。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B/course2"
"$B/course2/rsim" --selftest
"$W/course2/scripts/course2-verify.sh" "$B/bin" "$B/course2/rsim"
```

第 09 章把门禁从 135 项扩到 **186 项**：

| 来源 | 数量 |
| --- | --- |
| Zbb 语料逐行对拍 | 19 |
| Zbs 语料逐行对拍 | 8 |
| A 语料逐行对拍（aq/rl 归一化） | 20 |
| `muldiv` 的 `--isa=rv32i` / `--isa=rv32im` / 严格模式 / 坏 spec | 4 |

其中“严格模式”指：`muldiv_m`（含真实 `mul`）在 `--isa=rv32i` 下必须以
**126** 退出，并在 stderr 留下 `not enabled by --isa`。

## 预期输出

```text
$ rsim --run muldiv --isa=rv32i
rsim: halted at 0x00010040 after 856 instructions (exit 0, a0=0x00000000, t0=0x00000000)
rvmin: ok
$ echo $?
0

$ rsim --run muldiv_m --isa=rv32i
rsim: trap: illegal instruction: tval=0x02c516b3 epc=0x0001014c (instruction not enabled by --isa)
$ echo $?
126

$ rsim --run muldiv --isa=rv32
rsim: bad --isa spec rv32
usage: ...
$ echo $?
2
```

## 常见坑

1. **`--isa=rv32i` 被当成未知选项**。本章初版只实现了 `--isa <spec>`，
   而门禁脚本按常见写法传等号形式，结果 `arg[0] == '-'` 分支直接退出 2；
   表面症状是“`muldiv` 在 `--isa=rv32i` 下失败”，很容易误判成 `parseIsa` 的 bug。
   现在两种写法都接受，门禁同时覆盖。
2. **把 aq/rl 当成不同指令**。`llvm-mc -disassemble` 会打印 `lr.w.aqrl`，
   我们折叠成 `lr.w`；不做归一化会得到 4 倍的假 diff。
3. **Zbb 的一元指令按 R 型译码**。它们没有 `rs2`，`funct7` 字段其实是
   立即数的一部分；照抄 `add` 的模板会永远匹配不上。
4. **`match_imm` 的类型**。它是 `uint16_t`（第 08 章从 `uint8_t` 扩过），
   `0x600/0x698` 才放得下；写成 `uint8_t` 会静默截断成错误匹配值。
5. **拿 QEMU 对拍 Zbb**。QEMU 6.2.0 的 `rv32` 不认 `clz/rev8`，
   会直接 `SIGILL`；这两组只与 `llvm-mc` 对拍。
6. **以为 `--isa` 会影响 ELF 加载**。它只过滤执行阶段的指令；
   加载、符号表、反汇编仍然接受任意合法编码。

## 练习

1. 给 `--isa` 增加 `c`（压缩指令）位，先只做“拒绝/接受”的占位。
2. 实现 `--isa=rv32g` 的别名展开，并让 `--version` 打印当前默认掩码。
3. 用 `--isa=rv32i_zbb` 重跑 `demo`，确认它本来就不含 Zbb 指令（应当照常通过）。
4. 把 `isaOf()` 暴露成 `--dump-isa`，输出每个扩展的指令数（应得 RV32I/Zicsr=48、M=8、Zbb=17、Zbs=8、A=11）。
5. 给 `sc.w` 实现“失败返回 1”的保留集语义，并用 `lr.w/sc.w` 写一个自旋锁自测。
