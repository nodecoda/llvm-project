# 第 07 章：断点、单步与脚本调试

## 目标

给模拟器装一个**可脚本化**的调试器，并让“调试器能用”变成可执行的定义：

- `b <符号|地址>`：断点，符号从 ELF 的 `.symtab` 解析；
- `c`：继续到断点 / 程序退出 / trap；
- `s [n]`：单步 n 条（默认 1）；
- `r`：打印 32 个寄存器；
- `x/<n>xw <地址>`：按字查看内存，地址可以写寄存器名；
- `q`：退出。

`--script` 跑一段命令，输出就是 **transcript**；门禁用 `cmp` 逐字节比对 golden 文件。

## 背景：为什么不用 gdb

本机实测：

```text
$ gdb --batch -ex 'set architecture riscv:rv32'
Undefined item: "riscv:rv32".
```

没有 RISC-V 目标支持的 gdb 只能看 ELF 头，连单步都做不到。所以课程自带调试器，
并且把它设计成**输出稳定**：golden 文件是契约，任何输出顺序的变化都算回归。

顺带引入本章的新地基：`.symtab` 解析。没有它，`b main` 只能写死地址，
教程里每换一次编译选项就要改一次脚本。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `rsim/elf.cpp` | `SHT_SYMTAB` + 字符串表 → `Image::symbols` |
| `rsim/debugger.h` / `debugger.cpp` | 命令解析、断点、单步、寄存器、内存 |
| `rsim/main.cpp` | `--debug <elf> [--script <file>]` |
| `tests/debug-session.txt` | 会话脚本 |
| `tests/debug-session.golden` | 逐字节期望输出 |

## 关键实现

### 1. 符号表：连 `STT_NOTYPE` 也要收

`.symtab` 的每个条目有类型：`FUNC`、`OBJECT`、`NOTYPE`……手写汇编里的
`_start` 只有 `.globl`、没有 `.type`，所以它是 `NOTYPE`。第一版只收
`FUNC/OBJECT`，于是 `describe(0x10000)` 返回空串，调试器认不出程序入口。
现在收 `type <= STT_FUNC`，并跳过 `SHN_UNDEF`（未定义符号）。

### 2. 断点的“当前 pc”语义

这是全章最容易写错的地方：

| 场景 | 期望 |
| --- | --- |
| `b 0x10000`（pc 也在 `0x10000`）然后 `c` | **停住**，一条都不执行 |
| 在 `0x10000` 停下后 `c` | **越过**这条断点，继续跑 |

实现上用一个 `skip_once` 标志：只有当“上一次报告过的停止地址 == 当前 pc”时才跳过
一次断点检查。第一版写成了“`c` 永远先执行一条”，结果 `b 0x10000` 之后 `c`
直接跑完整个程序——门禁里的第二条检查就是为它准备的。

### 3. 输出即契约

- 提示符固定 `(rsim) `；
- 脚本模式下**回显命令**，这样 transcript 读起来像一次真实会话；
- 所有地址都打印成 `0x%08x`，寄存器 4 个一行，内存 4 个字一行；
- 符号显示为 `<main>` 或 `<main+4>`，绝不打印主机相关的信息。

golden 因此可以在不同机器上复现（demo 是静态 ELF，地址固定）。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
"$W/course2/scripts/course2-verify.sh" "$B/bin" "$B/course2/rsim"
```

第 07 章新增 2 项检查（总数 130 → 132）：

1. `tests/debug-session.txt` 的 transcript 与 golden **逐字节相同**；
2. 第二条会话验证 `b _start` 能解析符号、`c` 在“当前 pc 的断点”上先停、
   `s 2` 恰好前进两条。

## 预期输出

```text
$ rsim --debug demo --script tests/debug-session.txt
(rsim) b main
breakpoint 0: 0x00010180 <main>
(rsim) c
stopped at 0x00010180 <main>
(rsim) s
stopped at 0x00010184 <main+4>
(rsim) r
x0(zero)=0x00000000  x1(ra)=0x0001000c  x2(sp)=0x7fffefe0  x3(gp)=0x00000000
...
(rsim) x/4xw 0x7ffff000
0x7ffff000: 0x00000000 0x00000000 0x00000000 0x00000000
(rsim) q
quit
```

## 常见坑

1. **`b 0x10000` + `c` 跑飞**。断点位于当前 pc 时，`c` 必须先检查、后执行；
   第一版无条件先走一条，golden 会话没暴露它，第二条会话才抓到。
2. **`_start` 认不出来**。`.globl` 不带 `.type`，符号类型是 `NOTYPE`，
   过滤太严就只剩 `main`。判断条件是 `type <= STT_FUNC`。
3. **golden 里是绝对地址**。`0x00010180` 来自 demo 的固定链接布局；换编译器、
   改 `link.ld` 或加 `-O` 级别都会让 golden 失效——那时要重新生成并在提交里说明。
4. **`x/4xw sp` 忘了支持寄存器名**。地址解析先试寄存器、再试符号，
   否则脚本只能写死 `0x7ffff000`，栈一挪就废。
5. **把调试器输出写进 stdout 却又要 debug guest 的 stdout**。本章的会话不执行
   `write`，所以没有冲突；真要混跑，应当给调试器单独的输出流。

## 练习

1. 加 `d <n>` 删除断点、`bl` 列出断点，并让输出仍然可 golden 化。
2. 加 `p <表达式>`：至少支持 `$pc`、`$sp`、`<symbol>` 和 `+/-` 偏移。
3. 实现“断点命中计数”：第 N 次命中才停下（`ignore <n> <count>`）。
4. 用调试器验证 `fib(10)` 的调用栈深度：在 `fib` 下断点，统计连续命中次数与
   `sp` 的变化。
