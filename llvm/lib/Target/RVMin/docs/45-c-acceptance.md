# 第 45 章：第五篇验收：C 全链路

## 目标

第 43、44 章各交付了一半：MC 层能汇编/反汇编 35 条 `c.*`，AsmPrinter 能把
可压缩的 32 位指令投影成 16 位。本章不加新功能，只回答一个问题：

> 第五篇的承诺，能不能用可执行证据逐条兑现？

完成之后：

1. 编码门禁覆盖 **85 RV32I + 12 M + 19 Zbb + 8 Zbs + 20 A + 28 F + 28 D
   + 35 C**，全部与 `llvm-mc -triple=riscv32` 逐字节相同；
2. 反汇编门禁覆盖 **173 + 35 C + 6 混排 + 6 标签**，往返字节相同；
3. e2e 与 clang 矩阵各 **288 个对象**
   （12 程序 × 4 优化级 × 6 特性集 / 6 个 ISA）；
4. `rv32ic` 与 `rv32imafdc` 的 demo 在 QEMU 下输出与 RV32I 完全一致；
5. 同一份 IR 的 `+c` 构建 `.text` 从 **784 字节缩到 592 字节**；
6. 不开 C 的路径不变：192 个基线对象、编码/反汇编计数、`c-ext.ll` 的
   `NOC` 前缀共同锁定；
7. `docs/43–45` 八要素齐全，索引收尾，`rv32gc` 的对外口径固定。

## 背景：计划口径与实测的五处偏差

计划 `.omx/plans/2026-11-17-rvmin-c-extension.md` §7 写的是“C 语料预期
40±5 条”“45 章更新计数”。实测需要五处澄清：

1. **C 语料是 35 条，不是 40。** RV32C 的合法助记符就这么多（整数 27 +
   F 4 + D 4）；`c.addiw/c.ld/c.sd/c.ldsp/c.sdsp/c.subw/c.addw` 是 RV64
   专属，本章明确不做。
2. **门禁计数在 44 章就已更新。** 288 对象矩阵是 44 章交付的一部分，45 章
   负责**验收**它，而不是再改一遍脚本。本章的可执行证据全部来自 44 章末
   的同一套门禁。
3. **压缩只发生在 AsmPrinter。** `getInstSizeInBytes()` 仍返回 4，所以
   MachineFunction 层的尺寸统计会偏大；“784 → 592”是用 `objcopy` 量真实
   `.text`，不是用 `-stats`。
4. **`c.lui` 的立即数在 44 章被改成宽形式。** 43 章的 `simm6` 无法表达
   `lui a0, 1048575`（MachineOperand 是 -1），44 章引入 `c_lui_imm` 统一
   了谓词、打印与反汇编。这是对 43 章的一处有意修正，43 章文档已同步。
5. **`rv32gc` 仍然被拒绝。** `g` 蕴含 `zicsr/zifencei`，而 CSR 指令是
   第 46 章的内容；driver 在 43 章显式拒绝 `g`，这个拒绝要到 46 章才删除。
   “支持 C”不等于“支持 gc”。

另一个口径问题：**QEMU 不验证编码。** 它执行的是自己解码后的语义，字节级
正确性由编码门禁（对照 riscv32）和反汇编往返门禁保证。QEMU 在这里证明的是
“压缩后的程序语义没变”，两者缺一不可。

## 代码地图

| 位置 | 职责 |
| --- | --- |
| `scripts/rvmin-c-encoding-corpus.s` | 35 条 C 编码语料（整数 + F + D） |
| `scripts/rvmin-mixed-encoding-corpus.s` | 16/32 混排语料（6 条） |
| `scripts/check-rvmin-encodings.sh` | C 段：35 条对照 + 缺 C 诊断 |
| `scripts/check-rvmin-disasm.sh` | `roundtrip_c`（2 字节分组）+ `roundtrip_mixed` |
| `scripts/rvmin-e2e.sh` | 6 特性集：`""/+m/+f/+f,+d/+c/+m,+f,+d,+c` |
| `scripts/rvmin-clang-matrix.sh` | 6 ISA：`rv32i/im/if/id/ic/imafdc` |
| `scripts/rvmin-run.sh` | 压缩 demo 的运行 + `.text` 收缩断言 |
| `scripts/rvmin-verify-all.sh` | 8 项门禁；第 6 项加 `+c`，第 8 项含 C |
| `llvm/test/MC/RVMin/c-extension.s` | 35 条编码/反汇编/标签/诊断 |
| `llvm/test/CodeGen/RVMin/c-ext.ll` | 压缩取舍 + 非 C 路径不变 |
| `docs/43-c-extension-mc.md` | MC 层（第 43 章） |
| `docs/44-c-compression.md` | 压缩代码生成（第 44 章） |
| `docs/45-c-acceptance.md` | 本章 |

## 关键实现

### 1. 门禁分层

`rvmin-verify-all.sh` 仍是 8 项，成本递增：

```text
1  target 注册
2  lit（CodeGen + MC）
3  编码：35 条 C 对照 riscv32
4  反汇编：35 条 C + 6 条混排 + 6 条标签
5  e2e：12 × 4 × 6 = 288 对象
6  -verify-machineinstrs：每测试 × 4 O × {rv32i, +m, +c}
7  TableGen DSL 契约 + 状态矩阵 + 字段效果
8  QEMU：RV32I/M/A + F/D + 压缩 demo
```

### 2. “是否真的压缩”看原始字节列

`llvm-objdump` 默认打印别名（`c.mv` 显示成 `mv`，`c.swsp` 显示成 `sw`），
所以 `rvmin-clang-matrix.sh` 用原始字节宽度判定：

```bash
# 16 位指令的原始字节列是 4 个 hex（c606），32 位是 8 个（000002b7）
grep -Eq '^[[:space:]]+[0-9a-f]+:[[:space:]]+[0-9a-f]{4}[[:space:]]'
```

并在 `rv32ic`/`rv32imafdc` 上要求至少命中一次、其余 ISA 上要求零命中。

### 3. 非 C 路径的锁定

三条独立证据：

- `c-ext.ll` 的 `NOC` 运行：不开 `+c` 时 `grep -qE '^[[:space:]]+c\.'` 必须
  失败，且 `addi/jalr` 展开形式存在；
- e2e 的前 4 个特性集（192 对象）与 42 章完全相同的计数和 `.text` 对照；
- 编码/反汇编门禁的 RV32I/M/Zbb/Zbs/A/F/D 计数未变。

压缩发生在 AsmPrinter 且第一步就检查 `FeatureStdExtC`，所以“不开 C”在
代码路径上是恒等变换，不是靠 diff 碰巧相等。

### 4. 运行验收

`rvmin-run.sh` 在原有 RV32I/M 之后新增：

- `rv32ic/ilp32` 构建：输出必须与 RV32I 完全一致，汇编必须含 `c.*`；
- `rv32imafdc/ilp32d` 构建：同上，覆盖 M+F+D+C 同时打开；
- 同一份 IR 分别用 `-mattr=` 与 `-mattr=+c` 汇编，`.text` 必须严格变小。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B" llc clang llvm-mc llvm-lit llvm-objdump

# 8 项门禁（第 8 项需要 ld.lld 与 qemu-riscv32-static）
bash "$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"

# 单独看 C 的几项
bash "$W/llvm/lib/Target/RVMin/scripts/check-rvmin-encodings.sh" "$B/bin"
bash "$W/llvm/lib/Target/RVMin/scripts/check-rvmin-disasm.sh" "$B/bin"
bash "$W/llvm/lib/Target/RVMin/scripts/rvmin-e2e.sh" "$B/bin"
bash "$W/llvm/lib/Target/RVMin/scripts/rvmin-clang-matrix.sh" "$B/bin"
bash "$W/llvm/lib/Target/RVMin/scripts/rvmin-run.sh" "$B/bin"
```

## 预期输出

```text
PASS: 85 RV32I + 12 M + 19 Zbb + 8 Zbs + 20 A + 28 F + 28 D + 35 C instructions, .text identical
PASS: c (35 compressed instructions, .text round-trips byte-for-byte)
PASS: mixed (6 instructions, mixed 16/32-bit .text round-trips)
PASS: 173 encoding-corpus instructions + 35 compressed + 6 mixed + 6 label-resolved branches/jumps
PASS: 12 programs x 0 1 2 3 optimization levels x 6 feature sets
      = 288 objects, every .text identical to its llvm-mc reassembly
PASS 6/8: 204 llc runs with -verify-machineinstrs
PASS: compression shrank demo .text from 784 to 592 bytes
rv32ic: rvmin: ok
rv32imafdc: rvmin: ok
ALL 8 GATES PASS
```

clang 矩阵：

```text
ALL CLANG MATRIX PASS: 288 objects, ELF32 EM_RISCV, -S/-c agree, .text matches the riscv32 TargetInfo baseline
```

## 验收表

| ID | 标准 | 证据 | 结果 |
| --- | --- | --- | --- |
| C-AC1 | 35 条 C 编码与 `riscv32 +c` 逐字节相同 | 编码门禁 C 段 | 通过 |
| C-AC2 | FP C 变体在 `+c,+f,+d` 下同样一致 | 同一语料（28 条含 F/D） | 通过 |
| C-AC3 | 缺 C/F/D 分别报 `'C'/'F'/'D'` | `c-extension.s` 的 MISSING/MISSINGF/MISSINGD | 通过 |
| C-AC4 | 16 位反汇编无 unknown，16/32 混排往返字节相同 | `roundtrip_c` + `roundtrip_mixed` | 通过 |
| C-AC5 | `c.beqz/c.bnez/c.j/c.jal` 标签正确，越界/奇地址报错 | `c-extension.s` 的 LABEL/RANGE/ODD | 通过 |
| C-AC6 | objdump 不吞后续 32 位指令 | 混排语料 + 字节列断言 | 通过 |
| C-AC7 | `+c` 的 `.text` 严格小于无 C | `rvmin-run.sh`：784 → 592 | 通过 |
| C-AC8 | llc 汇编输出可重汇编且 `.text` 相同 | e2e 288 对象 | 通过 |
| C-AC9 | `+c` 程序在 QEMU 下运行通过 | `rv32ic`/`rv32imafdc` demo | 通过 |
| C-AC10 | `rv32ic`/`rv32imafdc` 可编译；`rv32gc` 明确拒绝 | clang 矩阵 + driver 诊断 | 通过 |
| C-AC11 | 不开 C 时输出与第 42 章一致 | `c-ext.ll` NOC + 192 基线对象 + 计数不变 | 通过 |
| C-AC12 | 8/8 门禁、文档 43–45 八要素、索引更新 | `rvmin-verify-all.sh` + 本索引 | 通过 |

## 常见坑

1. **不要用助记符判断“是否压缩”。** `llvm-objdump` 打印别名，`c.mv` 会
   变成 `mv`；看原始字节列的宽度（4 hex = 16 位）才可靠。
2. **`c.lui` 的两个写法是同一个字段。** 汇编器写 `1048575`，DAG 里是 -1；
   谓词、打印、反汇编必须统一，否则 `-S` 的输出无法再被汇编。
3. **`-mattr=+c` 和 `-march=rv32ic` 要成对出现。** `llc` 不读 IR 的
   `target-features`，只认命令行；clang 的 `-march` 决定 IR 与 ABI，
   两处不一致时会出现“IR 说 ilp32d、后端却是 rv32i”的假失败。
4. **`getInstSizeInBytes()` 不是压缩收益的度量。** 它仍按 4 字节返回；
   量真实大小要用 `objcopy --only-section=.text`。
5. **QEMU 通过不代表编码正确。** 模拟器执行的是它自己的解码结果；字节级
   一致性只能靠编码门禁与反汇编往返。
6. **`rv32gc` 被拒绝是刻意的。** 43–45 章没有 Zicsr/Zifencei；46 章才会
   删除 driver 里的 `g` 拒绝。
7. **混排反汇编不能按 4 字节分组。** `llvm-mc -disassemble` 按行解码，
   16 位语料要 2 字节一行；混排流走 `objdump → 文本 → 重汇编`。

## 练习

1. 给 ELF 写入 `EF_RISCV_RVC`（`e_flags` bit 0），并在 `llvm-readelf -h`
   上验证；讨论链接器/加载器会如何使用这个位。
2. 实现 `getCompressedSize()`，让 `getInstSizeInBytes()` 返回真实尺寸，
   再测 `.text` 是否变化（预期不变），解释为什么 MachineFunction 层的
   尺寸仍可能影响布局启发式。
3. 打开 linker relaxation（`R_RISCV_RELAX` + `-mrelax`），观察压缩后的
   分支在链接期被进一步缩短的收益与风险。
4. 让汇编器在 `-mattr=+c` 时把任意可压缩的 32 位助记符自动展开成 `c.*`
   （提示：`uncompressInst` 的反向用法），并与 `llc` 的压缩结果比较。
5. 统计 `scripts/e2e/*.c` 每个程序的压缩率，找出最低的一个，分析原因是
   寄存器不在 x8–x15、立即数越界，还是根本没有可压缩指令。

## 产物清单

- 文档：`docs/43-c-extension-mc.md`、`docs/44-c-compression.md`、
  `docs/45-c-acceptance.md`。
- 测试与门禁：`c-extension.s`、`c-ext.ll`、C 语料 2 份、编码/反汇编 C 段、
  288 对象矩阵、压缩 QEMU demo。
- tag：`rvmin-ch45`。
