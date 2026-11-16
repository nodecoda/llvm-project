# 第 27 章：用 clang 一条命令链接并运行

## 目标

第 26 章结束时，driver 已经能把 C 编成对象。本章把它推到终点：

```bash
clang --target=rvmin-unknown-elf -march=rv32i -mabi=ilp32 -O2 \
  -ffreestanding -fno-builtin -nostdlib -fuse-ld=lld \
  -Wl,-T,rt/link.ld rt/start.S rt/rvmin-rt.S rt/demo.c -o demo.elf

qemu-riscv32-static demo.elf
# rvmin: ok
```

不止 `demo.c`：`large-frame.c`、`tail.c`、`switch.c`、`muldiv.c`、
`atomics.c` 都通过同一条 driver 路径构建、链接、运行。新增门禁脚本：

```text
scripts/rvmin-clang-run.sh
```

它与 `rvmin-run.sh` 的分工：

| 脚本 | 目的 | 路径 |
| --- | --- | --- |
| `rvmin-run.sh` | 逐层验证 clang/llc/llvm-mc | `clang(riscv32) → llc -march=rvmin → llvm-mc -triple=rvmin → ld.lld` |
| `rvmin-clang-run.sh` | 验证用户实际会用的命令 | `clang --target=rvmin → ld.lld → qemu` |

## 背景：为什么一条命令就够了

三块拼图在前三章已经就位：

1. **TargetInfo**（第 25 章）提供 ILP32 布局、ABI 与 feature 白名单；
2. **driver**（第 26 章）选中 BareMetal、翻译 `-march/-mabi`、给出
   `-m elf32lriscv`；
3. **集成汇编器**能直接吃 `clang -S` 的输出（第 24 章的 `llvm-mc
   -triple=rvmin` 已覆盖同一 MC 层）。

剩下的只是运行时文件：`start.S`、`rvmin-rt.S`、`link.ld` 从第 12 章起
就没变过。本章**不改运行时**，只证明 clang 能驱动它们。

## 代码地图

| 文件 | 改动 |
| --- | --- |
| `scripts/rvmin-clang-run.sh` | 新脚本：`build_and_run` + 6 组程序 |
| `docs/27-clang-run.md` | 本章 |

脚本结构：

```text
build_and_run <program> <march> <opt> <output>
  clang --target=rvmin ... start.S rvmin-rt.S <program>.c -o elf
  llvm-readelf -h elf | grep 'Machine:.*RISC-V'
  qemu-riscv32-static elf > output
```

任何非空 stderr 都会让脚本失败——这同时锁住了“driver 不再打印
`-riscv-add-build-attributes`/未知 feature 警告”这件事。

## 关键实现

### 1. 链接参数

```bash
clang --target=rvmin-unknown-elf \
  -march=rv32i -mabi=ilp32 -O2 \
  -ffreestanding -fno-builtin -nostdlib \
  -fuse-ld=lld -Wl,-T,"$RT/link.ld" \
  "$RT/start.S" "$RT/rvmin-rt.S" "$RT/demo.c" \
  -o "$elf"
```

- `-nostdlib`：rvmin 没有 libc/启动文件，入口由 `start.S` 提供；
- `-fuse-ld=lld`：BareMetal 默认可能找 `ld`，显式选 lld 才能得到
  `-m elf32lriscv`；
- `-Wl,-T,link.ld`：链接脚本把 `.text` 放到 QEMU user-mode 可加载的地址；
- 源文件混排 `.S` 与 `.c`：driver 会分别走 `-cc1as` 与 `-cc1`，最终一起链接。

### 2. 两套 feature

```bash
-march=rv32i    # MUL/DIV/REM 走 rvmin-rt.S 的 __mulsi3 等
-march=rv32im   # 单条 mul/div/rem
-march=rv32ia   # atomics.c 需要 lr.w/sc.w/amo*
```

`demo.c` 与 `muldiv.c` 在两套 ISA 下都必须打印相同的文本；脚本用
`cmp` 断言输出逐字节一致。

### 3. 覆盖的程序

| 程序 | 选项 | 证明什么 |
| --- | --- | --- |
| `demo.c` | `rv32i`、`rv32im` | 基本算术、调用、运行时、输出一致 |
| `large-frame.c` | `-O0`、`-O2` | >2 KiB 帧与 RegScavenger |
| `tail.c` | `-O2` | 1e6 层尾调用不爆栈 |
| `switch.c` | `-O2` | 跳转表 + 间接跳转 |
| `muldiv.c` | `rv32i`、`rv32im` | 常量乘除强度削减 |
| `atomics.c` | `rv32ia` | A 扩展在 QEMU 上真实执行 |

`tail.c` 故意只跑 `-O2`：`-O0` 下每层 16 字节 × 1e6 会超过 8 MiB 栈并
SIGSEGV，这是第 17 章就写进合同的反例。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B" clang llvm-readelf

# 一键门禁
"$W/llvm/lib/Target/RVMin/scripts/rvmin-clang-run.sh" "$B/bin"

# 手工最小复现
RT="$W/llvm/lib/Target/RVMin/scripts/rt"
"$B/bin/clang" --target=rvmin-unknown-elf -march=rv32i -mabi=ilp32 -O2 \
  -ffreestanding -fno-builtin -nostdlib -fuse-ld=lld \
  -Wl,-T,"$RT/link.ld" "$RT/start.S" "$RT/rvmin-rt.S" "$RT/demo.c" \
  -o /tmp/demo.elf
qemu-riscv32-static /tmp/demo.elf
```

## 预期输出

```text
PASS: clang --target=rvmin linked and ran demo.c as rv32i and rv32im
PASS: 3 KiB frames ran under qemu-riscv32-static at -O0 and -O2
PASS: 1e6 tail calls ran under qemu-riscv32-static
PASS: jump-table switch program ran under qemu-riscv32-static
PASS: constant multiply/divide ran with and without M
PASS: A-extension atomics ran under qemu-riscv32-static
ALL CLANG-DRIVER RUNS PASS
```

最小复现：

```text
rvmin: ok
```

## 常见坑

1. **`-nostdlib` 不是可选项。** 不加会去找 `crt1.o`/`-lc`，BareMetal 的
   搜索路径里没有，链接直接失败；rvmin 的入口在 `start.S`。
2. **必须显式 `-fuse-ld=lld`。** 否则 driver 可能调用系统 `ld`，而
   `elf32lriscv` 的 emulation 与链接脚本行为不保证一致。
3. **`-Wl,-T` 的参数要用逗号或 `=` 传给 clang。** `-Wl,-T,path` 与
   `-Wl,-T=path` 都可；直接把 `-T path` 写在 clang 命令行会被当成输入文件。
4. **`-march=rv32ia` 的 `a` 会带上 `zalrsc/zaamo`。** 第 25 章的白名单
   必须允许这些隐含扩展，否则 atomics 程序会在 TargetInfo 层被拒。
5. **stderr 为空也是门禁。** 未知 feature、build-attributes 之类的警告不会
   让编译失败，但会让脚本的 `[[ -s "$elf.log" ]]` 触发；这是第 26 章
   feature 过滤的回归测试。
6. **`tail.c -O0` 一定 SIGSEGV。** 不要把它当失败；这是“尾调用真的生效”
   的反证。

## 练习

1. 把 `build_and_run` 的 `-O2` 改成 `-Os`，重跑六组程序；记录哪些输出
   或退出码变化，并解释是 IR 优化还是后端选择造成的。
2. 给脚本加一个 `--keep-temps` 选项，保留 ELF 与 `llvm-readelf -S` 输出，
   观察 `.text`/`.rodata`/`.eh_frame` 的地址是否落在 `link.ld` 声明的区间。
3. 用 `clang --target=rvmin -march=rv32i -###` 对比“同一程序分别以
   `-c` 编译再链接”与“源文件一次链接”的 cc1as 参数，确认 `.S` 与 `.c`
   走的是两条不同的 job。
