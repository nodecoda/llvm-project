# 第 06 章：跟踪与自反汇编

## 目标

给模拟器装两只“眼睛”，都用同一张 `insn.def`：

- `rsim --disas <elf>`：把可执行段逐字反汇编，输出 `地址 机器字 汇编`；
- `rsim --run <elf> --trace`：每执行一条就打印一行 `T 地址 机器字 汇编`；
- 两者对同一地址的文本**逐字符相同**——因为它们调用的是同一个 `format()`。

本章引入了课程里最有价值的一条证据链：**trace 的地址集合必须是
`llvm-objdump -d` 的子集，且 trace 行数必须等于 CPU 自己报告的指令数**。

## 背景：两个 LLVM 工具说的不是同一种“汇编”

同一个 `addi t0, a0, 0`，两个工具的输出：

```text
$ llvm-mc -triple=rvmin -disassemble demo.text.hex
addi t0, a0, 0

$ llvm-objdump -d demo
   1000c: 00050293     	mv	t0, a0
```

差别有两处：`llvm-objdump` **默认打印别名**（`mv/li/j/ret`），并且**默认用十六进制**
立即数；`llvm-mc -disassemble` 没有别名、十进制。而且 `llvm-objdump` 没有
`--no-aliases`（实测 `unknown argument`），只有 `--no-print-imm-hex`。

所以本章的门禁不能“拿两个文本 diff 了事”，而要分开取证：

| 比什么 | 拿谁比 | 为什么 |
| --- | --- | --- |
| 文本 | `llvm-mc -disassemble` | 无别名、十进制，与我们的 `format()` 同风格 |
| 地址/机器字 | `llvm-objdump -d` | 别名不影响机器字，覆盖范围最权威 |
| 执行路径 | trace | 只可能落在前两者给出的地址上 |

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `rsim/main.cpp` | `--disas`：遍历 `PT_LOAD` 中 `PF_X` 的段 |
| `rsim/cpu.h` / `cpu.cpp` | `traceTo(FILE*)`；`step()` 里一行 `fprintf` |
| `rsim/decode.cpp` | `format()`：唯二的文本来源（另一处是 `mnemonic()`） |

## 关键实现

### 1. trace 只有一处打印点

```cpp
if (trace_)
  std::fprintf(trace_, "T %08x %08x %s\n", at, word, format(d).c_str());
```

放在 `decode()` 之后、执行之前：这样即使这条指令随后 trap，trace 里也有它。
`T ` 前缀让门禁可以从容地把 trace 和 halt 摘要分开（两者都在 stderr）。

### 2. `--disas` 走段表，不走节表

ELF 加载器只认识 `PT_LOAD`（第 01 章的设计），所以反汇编也只看
`PT_LOAD && PF_X` 的段。对第一课产出的程序，可执行段恰好等于 `.text`
（实测 demo：段 `filesz=0x4f4`，`.text` 也是 `0x4f4`），因此可以和
`objcopy --only-section=.text` 的结果一一对齐。

### 3. 为什么 trace 输出到 stderr

第 05 章刚把 guest 的 stdout 变成“逐字节可比”的契约。trace 是模拟器的诊断，
走 stderr，差分测试才不会被它污染；同时 `--trace` 不影响 `--run` 的退出码。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
"$W/course2/scripts/course2-verify.sh" "$B/bin" "$B/course2/rsim"
```

第 06 章新增 3 项检查（总数 127 → 130）：

1. `--disas demo` 的文本与 `llvm-mc -disassemble` 逐行相同（317 条）；
2. `--disas demo` 的 `(地址, 机器字)` 与 `llvm-objdump -d` 完全相等；
3. `--trace` 的地址全部出现在 `--disas` 中，每行文本与 `--disas` 相同，
   且行数等于 halt 摘要里的 `after N instructions`。

## 预期输出

```text
$ rsim --disas demo | head -3
00010000 ff010113 addi sp, sp, -16
00010004 00112623 sw ra, 12(sp)
00010008 178000ef jal ra, 376

$ rsim --run demo --trace 2>&1 >/dev/null | head -3
T 00010000 ff010113 addi sp, sp, -16
T 00010004 00112623 sw ra, 12(sp)
T 00010008 178000ef jal ra, 376

$ rsim --disas demo | wc -l
317
$ $B/bin/llvm-objdump -d demo | grep -cE '^[[:space:]]+[0-9a-f]+:'
317
```

## 常见坑

1. **拿 `--disas` 和 `llvm-objdump` 的文本直接 diff**。第一条就红：
   `0x1000c` 我们写 `addi t0, a0, 0`，objdump 写 `mv t0, a0`。
   门禁因此改成“文本对 `llvm-mc`、机器字对 objdump”。
2. **以为 `llvm-objdump` 支持 `--no-aliases`**。实测直接报
   `unknown argument '--no-aliases'`；只有 `--no-print-imm-hex`。
3. **trace 打进 stdout**。第 05 章的 QEMU 差分立刻失败，且失败原因与程序无关。
4. **对 `tail` 开 trace**。1800 万行、约 1 GB；本章只用 `demo`（4027 条）取证。
5. **忘了 trace 是“执行到才算”**。`--disas` 覆盖 317 条，`demo` 只执行其中一部分
   （调用链之外的函数不执行），所以检查是“trace ⊆ disas”，不是相等。

## 练习

1. 给 `--trace` 加一个 `--trace-range <lo> <hi>`，只打印区间内的指令。
2. 统计 trace 里每个 `pc` 出现的次数，画出 demo 的热点（第 10 章会系统做）。
3. 让 `--disas` 支持 `PF_X` 之外的段，并解释为什么“反汇编数据段”通常没有意义。
4. 用 `--trace` 验证 `fib(10)` 的调用深度，并与源码里 `fib` 的递归次数对照。
