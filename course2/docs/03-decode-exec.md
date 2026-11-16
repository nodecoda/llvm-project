# 第 03 章：译码与执行 RV32I

## 目标

让 `rsim --run` 真正执行机器码：取指、译码、执行、更新 `pc`，直到 `ecall`。

本章结束时应能：

- 用**一张表**同时支撑译码、执行分派和反汇编文本，三者不可能漂移；
- 解释 RISC-V 的 B/J 型立即数为什么是“打散”的，并手写位重组；
- 说清 `x0`、移位量掩码、`sltiu -1`、`jalr` 的 LSB 这四处的语义；
- 用 `--selftest` 和 97 条真实编码两层证据证明“跑得对”。

## 背景：一条指令在真实 CPU 里发生什么

`pc` 指向一个 32 位字，硬件把它拆成字段：

```
 31        25 24    20 19   15 14  12 11     7 6        0
+------------+--------+-------+------+--------+----------+
|   funct7   |  rs2   |  rs1  |funct3|   rd   |  opcode  |
+------------+--------+-------+------+--------+----------+
```

模拟器做的事一模一样，只是用 C++ 的 `>>` 和 `&`。教学上唯一值得强调的：
**译码表是数据，不是代码**。`insn.def` 用 X-macro 描述“哪条指令长什么样”，
`decode.cpp` 把它展开成表；第 06 章的 `--trace` 再用同一张表生成汇编文本。
加一条新指令只需要改 `insn.def` 一行。

第一课（LLVM 后端）是“编码器”，第二课是“译码器”。两边用同一份语料
`rvmin-encoding-corpus.s` 对拍，就等于给课程装了一个双向的校准器。

## 代码地图

| 文件 | 行数 | 作用 |
| --- | --- | --- |
| `rsim/insn.def` | 71 | 49 条指令（RV32I 41 + M 8）的单一真源 |
| `rsim/decode.h` | 66 | `Fmt`/`Isa`/`Insn` 枚举、`Decoded`、`format()` |
| `rsim/decode.cpp` | 205 | 表驱动译码、B/J 立即数重组、汇编文本 |
| `rsim/cpu.cpp` | 300 | `step()`：取指 → 译码 → 执行 → `pc` |
| `rsim/selftest.cpp` | 300 | 65 条手算向量（含边角） |
| `rsim/main.cpp` | 200 | `--run` / `--decode-hex` / `--selftest` |

## 关键实现

### 1. X-macro：一份列表，三处展开

```c
// insn.def
RSIM_INSN(ADD, "add", FmtR, 0x33, 0x00, 0x00, 0xff, RV32I)
```

`decode.cpp` 至少展开三次：生成 `Insn` 枚举、生成 `kTable[]`、生成 `mnemonic()`。
最后一项尤其重要——第 06 章的反汇编不另写一份 switch，而是复用 `format()`，
所以“能译码”和“能反汇编”在构造上就是同一件事。

### 2. B/J 型立即数：位是打散的

B 型把 13 位偏移拆到四个位置，J 型把 21 位拆到五个位置，而且最低位永远隐含为 0：

```
B: imm[12]=31  imm[10:5]=30:25  imm[4:1]=11:8  imm[11]=7
J: imm[20]=31  imm[10:1]=30:21  imm[11]=20    imm[19:12]=19:12
```

写反了不会报错，只会跳到错误的地址——所以第 04 章还会单独用分支密集的程序
再验一次。本章的门禁里，97 条语料已经覆盖了正负偏移的编码；执行侧由
`--selftest` 的 8 条分支向量覆盖（含 `-4` 的后向分支）。

### 3. `step()` 就是一个 switch

```cpp
const uint32_t word = memory().read32(pc);   // 取指
const Decoded d = decode(word);              // 译码
if (d.id == Insn::ILLEGAL) trapf(...);       // 未实现的编码不是“空操作”
uint32_t next = pc + 4;                      // 默认顺序执行
switch (d.id) { ... }                        // 执行
pc = next;
```

所有写寄存器都走 `setReg()`，它在 `reg == 0` 时直接丢弃。把这条规则放在**一个**
入口，比在 49 个 case 里各写一次 `if (rd)` 可靠。

### 4. 四处容易错的语义

1. **`x0`**：写入被丢弃，但读取永远是 0（`reset()` 里已经清零，之后没人能改）。
2. **移位量**：寄存器版取 `rs2 & 31`，立即数版取 `imm & 31`；`sra/srai` 必须有符号。
3. **`sltiu`**：立即数先做 12 位符号扩展，再按**无符号**比较。
   所以 `sltiu rd, zero, -1` 的结果是 1（`0 < 0xFFFFFFFF`），不是 0。
4. **`jalr`**：目标地址的 bit0 先清零再写入 `pc`；链接值永远是“下一条指令”。

M 扩展的除法边界在规范里是**定义好的**，不是未定义行为：

| 情况 | `div` | `divu` | `rem` | `remu` |
| --- | --- | --- | --- | --- |
| 除数为 0 | `-1` | `0xFFFFFFFF` | 被除数 | 被除数 |
| `INT_MIN / -1` | `INT_MIN` | — | `0` | — |

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

cmake -S "$W/course2" -B "$B/course2" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$B/course2"
"$W/course2/scripts/course2-verify.sh" "$B/bin" "$B/course2/rsim"
```

门禁做了三件事：

1. `--selftest`：65 条手算向量全过；
2. 97 条真实编码（85 条 RV32I + 12 条 M）分别由 `rsim --decode-hex` 和
   `llvm-mc -disassemble` 译码，**逐行 diff**；
3. 第一课的 5 个程序用 `--run` 跑到第一个 `ecall`，要求 `main()` 的返回值
   （`start.S` 存在 `t0` 里）为 0。

语料的字节这样取——注意 `corpus.o` 是 **ET_REL**，第 01 章的加载器按设计拒绝它：

```bash
"$B/bin/llvm-mc" -triple=rvmin -filetype=obj corpus.s -o corpus.o
"$B/bin/llvm-objcopy" -O binary --only-section=.text corpus.o corpus.bin
xxd -p -c4 corpus.bin | awk '{printf "0x%s 0x%s 0x%s 0x%s\n", ...}' > corpus.hex
```

## 预期输出

```text
$ rsim --selftest
selftest: 65 checks, 0 failures

$ rsim --run demo --max-steps 10000000
rsim: halted at 0x00010034 after 4024 instructions (a0=0x00000001, t0=0x00000000)

$ course2-verify.sh $B/bin $B/course2/rsim
117 checks
```

5 个程序在本章的实测指令数（`-O2`，无 `+m` 的走 libcall）：

| 程序 | 指令数 | 说明 |
| --- | --- | --- |
| `muldiv`（`+m`） | 96 | 直接命中 `mul/div/rem` |
| `switch` | 144 | 跳转表 + 分支 |
| `demo` | 4 024 | `fib(10)`、`crc32`、libcall |
| `large-frame` | 927 999 | 大栈帧循环 |
| `tail` | 18 010 056 | 1e6 次尾调用（默认 10M 步上限不够，门禁给到 50M） |

## 常见坑

1. **I 型指令忘了填立即数**。`decode()` 的 switch 最初只给 load/jalr 赋了 `imm`，
   `addi/slti/xori/...` 落进 `default` 拿到 0。6 条 selftest 立刻变红，
   而 97 条语料里的偏移恰好都是 0 附近——**对拍工具不会告诉你执行错了**。
2. **单测夹具忘了重置 `pc`**。第一版 `Fixture::exec()` 只写指令不写 `pc`，
   第二次调用就取到 `0x00000000`，得到 `illegal instruction ... at 0x00001004`。
3. **把 `.o` 直接喂给 `--dump-elf`**。加载器只收 ET_EXEC（`elf.cpp:57`），
   语料必须走 `objcopy` 抽 `.text`。
4. **`llvm-mc -disassemble` 不吃二进制**，吃的是十六进制**文本**；
   传 `.bin` 会得到 `invalid input token`。
5. **`fence/fence.i` 不在 RV32I 语料里**。第一课把这两条放在 A 扩展下，
   `llvm-mc -triple=rvmin` 不加 `-mattr=+a` 会直接报错。本章的 `insn.def`
   仍然收录它们（执行是空操作），第 09 章再用 `+a` 单独验。
6. **`tail` 的默认步数上限**。18M 步 > 默认 10M，会被当成 trap；
   这不是死循环，是课程故意留的“上限要可配”提醒。

## 练习

1. 在 `insn.def` 里加一条 `sltiu` 的“别名”测试：把 `-1` 当作 `0xFFFFFFFF`
   写进语料，确认 `--selftest` 和 97 条语料都仍然一致。
2. 把 `step()` 改成返回枚举（`Continue/Halt/Trap`），为第 08 章的陷阱模型铺路。
3. 写一个 `--exec-hex`：读原始指令字，从 `0x1000` 开始执行，打印最终寄存器。
   想想它和 `--selftest` 的夹具重复了什么。
4. 用 `--max-steps 1` 跑 `demo`，观察 `pc` 停在哪里；再解释为什么
   “停在下一条指令之前”和“执行了一半”是两种不同的状态。
