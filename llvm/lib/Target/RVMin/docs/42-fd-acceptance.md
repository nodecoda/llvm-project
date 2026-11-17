# 第 42 章：第四篇验收：F/D 全链路

## 目标

第 40、41 章各加了一半浮点：F 让 f32 变成合法类型，D 让 f64 跟上。本章不加
新指令，只回答一个问题：**这两章的承诺，能不能用可执行证据逐条兑现？**

完成之后：

1. `rvmin-verify-all.sh` 从 7 项扩成 **8 项**，第 8 项是 QEMU 运行；
2. 编码门禁覆盖 **85 RV32I + 12 M + 19 Zbb + 8 Zbs + 20 A + 28 F + 28 D**；
3. e2e 与 clang 矩阵各 **192 个对象**（12 程序 × 4 优化级 × 4 特性集/ISA）；
4. `float.c` 在 `rv32if/ilp32f` 与 `rv32id/ilp32d` 下都能运行，
   `double.c` 在 `rv32id/ilp32d` 下运行，退出码为 0；
5. F/D 构建的汇编里不再出现 `__addsf3/__adddf3` 等软浮点调用；
6. `docs/40–42` 八要素齐全，索引收尾。

## 背景：计划口径与实测的三处偏差

计划（`.omx/plans/2026-11-17-rvmin-f-d-extension.md` §7）写的是“10 程序 ×
4 × 4 = 160 对象”“QEMU 输出与软浮点版本一致”。实测有三处需要改口径：

1. **e2e 程序是 12 个，不是 10 个。** 第 40 章加了 `fp.c`，第 41 章加了
   `fp-double.c`，所以 12 × 4 × 4 = 192。矩阵同理。
2. **freestanding 运行时没有软浮点 libcall。** `rvmin-rt.S` 只提供
   `__mulsi3/__divsi3/...` 这些整数例程；第 10 章的软浮点程序之所以能编译，
   是因为 e2e 只到目标文件为止，不链接运行。要让“软浮点版本运行起来”，
   得先实现 `__addsf3/__adddf3/__fixdfsi/...` 一整组例程，那是另一个课程单元。
   因此本章的 QEMU 口径改为：
   - `float.c` 分别用 `rv32if/ilp32f` 和 `rv32id/ilp32d` 构建，两者都必须
     返回 0（同一份源码、两种 ISA/ABI）；
   - `double.c` 只在 D 下运行；
   - 硬浮点构建必须**不引用**任何软浮点符号，F-only 构建不得出现 D 指令。
3. **“门禁 8/8”落在 `rvmin-verify-all.sh` 里。** 第 15 章曾把 QEMU 单独拆出去
   （需要链接器和模拟器）；验收章把它接回来：找不到 `qemu-riscv32-static` 或
   `ld.lld` 时明确报错，而不是悄悄少跑一项。

另外，第 41 章的 e2e 首跑抓到一个真实崩溃：DAGCombine 把
`store (fptrunc f64→f32)` 融合成截断 store、把 `(double)f` 融合成扩展 load，
而 RVMin 只有普通 4/8 字节访存 pattern，选择阶段 `Cannot select`。修复
（`setTruncStoreAction` / `setLoadExtAction` 成 `Expand`）已写进第 41 章，
并在 `d-ext.ll` 留了回归函数。这也是验收章存在的意义：**单元测试通过不等于
全量语料通过**。

## 代码地图

| 位置 | 职责 |
| --- | --- |
| `scripts/rt/float.c` | F 运行验收：9 个 float 参数、转换、比较、常量、select |
| `scripts/rt/double.c` | D 运行验收：9 个 double 参数、宽度转换、栈常量、select |
| `scripts/rvmin-run.sh` | 新增 `run_fp`；F 两次、D 一次，并做符号/指令断言 |
| `scripts/rvmin-verify-all.sh` | 第 8 项：QEMU 运行 |
| `scripts/rvmin-e2e.sh` | 4 个特性集（`""/+m/+f/+f,+d`） |
| `scripts/rvmin-clang-matrix.sh` | 4 个 ISA（`rv32i/rv32im/rv32if/rv32id`） |
| `scripts/check-rvmin-encodings.sh` | F/D 各 28 条 |
| `scripts/check-rvmin-disasm.sh` | F/D 各 28 条往返 |
| `llvm/test/MC/RVMin/f-extension.s`、`d-extension.s` | MC 层编码与诊断 |
| `llvm/test/CodeGen/RVMin/f-ext.ll`、`d-ext.ll` | 选择、ABI、软浮点回归 |
| `docs/42-fd-acceptance.md` | 本章 |

## 关键实现

### 1. 门禁分层：7 项静态 + 1 项运行

`rvmin-verify-all.sh` 的顺序按成本递增：

```text
1  target 注册
2  lit（CodeGen + MC，28 个测试）
3  编码：与 riscv32 参考汇编器逐字节比较
4  反汇编：asm -> obj -> disasm -> asm 往返
5  e2e：12 程序 × 4 -O × 4 特性集，.text 与 llvm-mc 重汇编一致
6  每个 CodeGen 测试 × 4 -O × {rv32i,+m}，-verify-machineinstrs
7  TableGen DSL 七个后端契约 + 状态矩阵 + 字段影响面
8  QEMU：RV32I/M/A + F/D float/double 程序
```

第 6 项只跑 `rv32i/+m` 是有意的：`+f` 与 `+f,+d` 的组合由 lit（每个测试自带
`-mattr`）与第 5 项覆盖，否则同一批测试要跑四遍，收益递减。

### 2. 运行程序的设计：退出码就是断言

没有 libc，就没有 `printf`。`float.c`/`double.c` 的 `main` 返回第一个失败检查
的编号，`rvmin-run.sh` 只接受 0：

```c
if (f1 + f2 != 3.75f)   return 1;
if (f2i(f1) != 1)       return 2;
...
```

为了让优化器**不能**把检查消掉，程序用了两层障眼法：

- 输入是 `volatile` 全局量；
- 转换放在 `__attribute__((noinline))` 的小函数里。

第二点是被实测逼出来的。最初写成 `if ((int)(f1+f2) != 3)`，clang 会利用前一条
`f1+f2 == 3.75` 的分支事实，把 `(int)` 直接折成常量 3，汇编里根本没有
`fcvt.w.s`。加了 helper 边界后，`x` 对函数体不透明，转换才留下来。

### 3. 每个程序证明了什么

`float.c`（F 与 D 各跑一次）：

| 检查 | 产生的指令 |
| --- | --- |
| `f1 + f2` / `f1 * f2` | `fadd.s` / `fmul.s` |
| `f2i(f1)` | `fcvt.w.s ..., rtz` |
| `i2f(ci)` | `fcvt.s.w` |
| `f1 < f2` | `flt.s` |
| `add9f(1..9)` | 第 9 个参数走栈；调用约定 |
| `f1*1.5f + 2.0f` | 常量经 `fmv.w.x` |
| `f1 > f2 ? f1 : f2` | f32 SELECT（GPR mask） |

`double.c`（仅 D）：

| 检查 | 产生的指令 |
| --- | --- |
| `d1 + d2` | `fadd.d` |
| `d2i(d1)` / `i2d(ci)` | `fcvt.w.d ..., rtz` / `fcvt.d.w` |
| `d1 < d2` | `flt.d` |
| `roundtrip(d1+d2)` | `fcvt.s.d` + `fcvt.d.s` |
| `add9(1..9)` | 9 个 double：第 9 个走栈，callee-saved 用 8 字节槽 |
| `d1*1.5 + 2.0` | f64 常量：8 字节栈槽 + 两条 `sw` + `fld` |
| `d1 > d2 ? d1 : d2` | f64 SELECT：两个槽 + 选地址 + `fld` |

运行脚本同时做静态断言：

```bash
# F-only 构建不得出现 D 指令
grep -q 'fadd.d\|fld\|fsd' f32-if.s && fail
# 硬浮点构建不得引用软浮点符号
for SYM in __addsf3 __adddf3 __extendsfdf2 __truncdfsf2 ...; do
  [[ "$RELOCS" != *"$SYM"* ]] || fail
done
```

### 4. clang 矩阵的 ISA 标记

矩阵按 `rv32i/rv32im/rv32if/rv32id` 四档编译同一个语料，并对反汇编做
“必须出现/必须不出现”的检查：

| ISA | 必须出现 | 必须不出现 |
| --- | --- | --- |
| `rv32i` | — | `mul`、`fadd.s` |
| `rv32im` | `mul` | `fadd.s` |
| `rv32if` | `fadd.s` | `fadd.d` |
| `rv32id` | `fadd.d` | — |

这四行把“扩展只影响自己那一档”变成可执行断言，也防止某天复制粘贴把
`-mattr` 写死。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B" llc clang llvm-mc llvm-lit

# 8 项门禁（第 8 项需要 ld.lld 与 qemu-riscv32-static）
bash "$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"

# 单独看运行门禁的输出
bash "$W/llvm/lib/Target/RVMin/scripts/rvmin-run.sh" "$B/bin"
```

外部依赖可以用环境变量覆盖：

```bash
QEMU=qemu-riscv32 LD=ld.lld \
  bash "$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"
```

## 预期输出

```text
PASS 1/8: target registered
PASS 2/8: lit (CodeGen + MC)
PASS 3/8: encodings
PASS 4/8: disassembler
PASS 5/8: e2e
PASS 6/8: 128 llc runs with -verify-machineinstrs
PASS 7/8: TableGen DSL contracts + status matrix + field effect
PASS 8/8: QEMU run (RV32I/M/A + F/D float and double programs)
ALL 8 GATES PASS
```

第 3、4、5 项的计数：

```text
PASS: 85 RV32I + 12 M + 19 Zbb + 8 Zbs + 20 A + 28 F + 28 D instructions, .text identical
PASS: d (28 instructions, .text round-trips byte-for-byte)
PASS: 173 encoding-corpus instructions + 6 label-resolved branches/jumps
PASS: 12 programs x 0 1 2 3 optimization levels x 4 feature sets
      = 192 objects, every .text identical to its llvm-mc reassembly
```

clang 矩阵：

```text
PASS: e2e corpus under -march=rv32if -O2
PASS: e2e corpus under -march=rv32id -O2
ALL CLANG MATRIX PASS: 192 objects, ELF32 EM_RISCV, -S/-c agree,
      .text matches the riscv32 TargetInfo baseline
```

QEMU 门禁：

```text
PASS: F/D programs ran under qemu-riscv32-static (rv32if + rv32id; 9-arg calls,
      f64 stack constants/select, and no soft-float libcalls)
```

## 验收表

| ID | 标准 | 证据 | 结果 |
| --- | --- | --- | --- |
| F-AC1 | F 28 条编码与参考汇编器逐字节相同 | `check-rvmin-encodings.sh` 的 F 段 | 通过 |
| F-AC2 | D 28 条编码逐字节相同 | 同上 D 段 | 通过 |
| F-AC3 | 缺 F/D 时报 `'F'/'D'` 诊断 | `f-extension.s`/`d-extension.s` 的 MISSING 检查 | 通过 |
| F-AC4 | 反汇编无 `<unknown>`，往返字节相同 | `check-rvmin-disasm.sh`：F 28 + D 28 | 通过 |
| F-AC5 | F 代码生成：算术/符号/最值/FMA/比较/转换/常量/select | `f-ext.ll` | 通过 |
| F-AC6 | D 代码生成：同上 + f64 栈常量/select + 9 参数 | `d-ext.ll`（含截断 store/扩展 load 回归） | 通过 |
| F-AC7 | 不开 F/D 时软浮点路径不变 | `f-ext.ll`/`d-ext.ll` 的 SOFT 前缀；e2e 的 `rv32i` 格 | 通过 |
| F-AC8 | ABI 校验：`ilp32+F`、`ilp32f+D`、`ilp32d 无 D` 全部报错 | `BADABI`/`NOD` lit + clang 诊断 | 通过 |
| F-AC9 | clang `rv32if/ilp32f`、`rv32id/ilp32d` 端到端 | `rvmin-clang-matrix.sh` 192 对象 | 通过 |
| F-AC10 | llc 输出可被本汇编器重汇编且 `.text` 相同 | `rvmin-e2e.sh` 192 对象 | 通过 |
| F-AC11 | F/D 程序在 QEMU 下运行，且无软浮点 libcall | `rvmin-run.sh` 的 `run_fp` | 通过 |
| F-AC12 | lit 全绿、DSL 契约同步、文档八要素 | `llvm-lit` 28/28；`dsl-surface.py --check`；本索引 | 通过 |

## 常见坑

1. **“软浮点版本输出一致”在 freestanding 下不可执行。** 没有 `__addsf3` 等
   例程，软浮点浮点程序根本链接不了。验收要么补运行时，要么改口径；本章选择
   后者，并把“无软浮点符号”作为硬断言。
2. **优化器会用控制流事实折叠转换。** `if (x == 3.75) ... (int)x ...` 里的
   `(int)x` 会被折成 3；必须用 `noinline` 边界或 volatile 汇点把值变不透明。
3. **`volatile` 只挡住折叠，不挡指令选择。** 它保证 load/store 真实存在，
   但算术仍由后端选指令；两者配合才能同时证明“指令出现”和“结果正确”。
4. **门禁计数会随语料增长漂移。** 计划写 160，实际 192；脚本里的计数是
   算出来的（`PROGRAMS`/`CASES`），不要在文档里硬编码后再忘了改。
5. **`verify-all` 现在依赖 `qemu-riscv32-static` 与 `ld.lld`。** 这是验收章的
   有意变化；缺工具时用 `QEMU=`/`LD=` 覆盖，或先只跑第 1–7 项。
6. **e2e 的 `.text` 相同不等于语义相同。** 它证明 llc 与 llvm-mc 一致；
   语义正确性由 QEMU 程序（退出码）和 lit 的 pattern 断言分别覆盖。
7. **同一份 IR 喂 `llc` 不等于 clang 的代码生成。** clang 会在 IR 里写
   `target-features`，而 `llc` 只认 `-mattr`；验收脚本必须两边同时给特性
   （这是第 28 章踩过的坑在本篇的翻版）。

## 练习

1. 给 `rvmin-rt.S` 补一组最小软浮点例程（`__addsf3/__mulsf3/__fixsfsi`），
   让 `float.c` 的纯软构建也能链接运行，然后把本章口径恢复成“三种构建输出
   一致”。
2. 把 `run_fp` 的静态断言从 grep 升级成“反汇编后统计指令直方图”，
   要求 F 构建的 D 指令计数为 0、D 构建的 `fcvt.*` 计数不小于预期。
3. 用 `-fsanitize=float-cast-overflow` 风格的思路，给 `fcvt.w.d` 加一个
   超出 int 范围的行为测试，比较 QEMU 与真实硬件/上游 LLVM 的输出。
4. 把第 6 项 `-verify-machineinstrs` 扩成 `{rv32i,+m,+f,+f,+d}`，测量门禁
   时间增长，并决定是否值得。
5. 给 `double.c` 增加一个“double 返回值跨调用活跃”的场景，验证
   `fs0–fs11` 的 8 字节槽在真实运行中不会被 `fsd` 越界写坏。

## 产物清单

- `scripts/rt/float.c`、`scripts/rt/double.c`
- `scripts/rvmin-run.sh`：`run_fp` + F/D 断言
- `scripts/rvmin-verify-all.sh`：第 8 项 QEMU 门禁
- `docs/42-fd-acceptance.md`
- `docs/README.md`、`llvm/lib/Target/RVMin/README.md`：第四篇收尾
