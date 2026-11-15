# 第 12 章：链接与运行（运行时、ecall 与 QEMU）

## 目标

前面的章节只到 `.o` 为止：能生成正确的机器码，但没有任何东西能**执行**它。
本章把它跑起来：

```c
/* 操作数来自 volatile 全局变量，编译器不能把它们折叠成常量 ——
   否则 __mulsi3 之类的调用根本不会出现，自检就成了空转。 */
volatile int vmul_a = 1000, vmul_b = 37;
volatile unsigned vdiv_a = 37000u, vdiv_b = 37u;
volatile int vsdiv_a = -37000, vsdiv_b = 37;
volatile int vmod_a = -37, vmod_b = 10;
volatile long long vshift = 1LL;
volatile int vcount = 40;

int main(void) {
  if (fib(10) != 55) return 1;                      /* 调用、递归、栈帧 */
  if (vmul_a * vmul_b != 37000) return 2;           /* __mulsi3 */
  if (vdiv_a / vdiv_b != 1000u) return 3;           /* __udivsi3 */
  if (vsdiv_a / vsdiv_b != -1000) return 4;         /* __divsi3 */
  if (vmod_a % vmod_b != -7) return 5;              /* __modsi3 */
  if ((vshift << vcount) != 1099511627776LL) return 7;  /* __ashldi3 */
  if (crc32((const unsigned char *)"123456789", 9) != 0xCBF43926u) return 10;
  return 0;
}
```

```console
$ rvmin-run.sh $B
rvmin: ok
PASS: linked 6656 bytes (rv32i), ran under qemu-riscv32-static, all checks green
```

（第 13 章会让同一个脚本再跑一遍 `+m` 构建，所以最终的输出是两行 `rvmin: ok`。）

完成本章后：

1. MC 层支持 `ecall`/`ebreak`（运行时唯一需要的“系统调用”接口）；
2. 有一个纯 RV32I 的最小运行时 `rvmin-rt.S`，实现 8 个 libcall；
3. 有 `start.S`（`_start`、`write`/`exit` 系统调用）与链接脚本；
4. `clang → llc → llvm-mc → ld.lld → qemu-riscv32` 全链路可复现。

## 代码地图

| 文件 | 内容 |
| --- | --- |
| `RVMinInstrInfo.td` | `ECALL`/`EBREAK`（固定 32 位编码） |
| `scripts/rvmin-encoding-corpus.s` | 两条 SYSTEM 指令进入编码门禁（83 → 85） |
| `scripts/rt/rvmin-rt.S` | `__mulsi3`/`__udivsi3`/`__umodsi3`/`__divsi3`/`__modsi3`/`__ashldi3`/`__lshrdi3`/`__ashrdi3` |
| `scripts/rt/start.S` | `_start`：调用 `main`、打印结果、`exit` |
| `scripts/rt/link.ld` | 从 `0x10000` 开始的静态布局 |
| `scripts/rt/demo.c` | 自检程序 |
| `scripts/rvmin-run.sh` | 编译 → 汇编 → 链接 → 运行的完整门禁 |

## 关键实现

### 1. `ecall`：一条没有操作数的 I 型指令

`ecall`/`ebreak` 的编码是 I 型，但 `rs1`、`rd` 都是 0，`imm` 分别是 0 和 1，所以
不需要任何操作数——**整个 32 位字是常量**：

```tablegen
def ECALL : RVMinInst<(outs), (ins), "ecall", []> {
  let Inst = 0x00000073;
  let isBarrier = 1;
}

def EBREAK : RVMinInst<(outs), (ins), "ebreak", []> {
  let Inst = 0x00100073;
}
```

`Inst` 是 32 位字段，直接赋常量就把所有位钉死了，TableGen 生成的编码器不需要
任何操作数取值。`isBarrier` 让 `ecall` 之后不能再被穿越（它可能不返回）。

与参考汇编器逐字节一致（第 2 章的门禁现在有 85 条指令）：

```
        ecall                           # encoding: [0x73,0x00,0x00,0x00]
        ebreak                          # encoding: [0x73,0x00,0x10,0x00]
```

在 QEMU 的 Linux 用户模式里，`ecall` 就是系统调用入口：`a7` 放调用号，
`a0`-`a5` 放参数，返回值在 `a0`。

### 2. 运行时：把 libcall 的 ABI 兑现

第 10 章让后端“发出”了这些调用，但没人提供实现。`rvmin-rt.S` 用**纯 RV32I**
实现它们（没有伪指令，也没有 M 扩展），所以它既能被我们的汇编器接受，也能被
任何 RISC-V 工具链接受：

```asm
// unsigned __mulsi3(unsigned a, unsigned b)
__mulsi3:
        addi    t0, zero, 0             // product
        beq     a1, zero, .Lmul_done
.Lmul_loop:
        andi    t1, a1, 1
        beq     t1, zero, .Lmul_shift
        add     t0, t0, a0
.Lmul_shift:
        slli    a0, a0, 1
        srli    a1, a1, 1
        bne     a1, zero, .Lmul_loop
.Lmul_done:
        addi    a0, t0, 0
        jalr    zero, 0(ra)
```

除法是无恢复余数的逐位算法（32 次迭代），商和余数各一份实现：

```asm
.Ludiv_loop:
        srli    t3, a0, 31              // 被除数的最高位
        slli    a0, a0, 1
        slli    t1, t1, 1
        or      t1, t1, t3              // remainder = (remainder << 1) | bit
        slli    t0, t0, 1               // quotient <<= 1
        bltu    t1, a1, .Ludiv_next
        sub     t1, t1, a1
        ori     t0, t0, 1
.Ludiv_next:
        addi    t2, t2, -1
        bne     t2, zero, .Ludiv_loop
```

有符号版本先用 `srai`+`xor`+`sub` 取绝对值，调用无符号核心，再修符号：

```asm
__divsi3:
        xor     t4, a0, a1              // 结果的符号
        srai    t5, a0, 31
        xor     a0, a0, t5
        sub     a0, a0, t5              // |a|
        ...
        addi    sp, sp, -16
        sw      ra, 12(sp)
        sw      t4, 8(sp)               // t4 是 caller-saved，跨调用要自己保存
        jal     ra, __udivsi3
        lw      t4, 8(sp)
        lw      ra, 12(sp)
        addi    sp, sp, 16
        bge     t4, zero, .Ldiv_done
        sub     a0, zero, a0
.Ldiv_done:
        jalr    zero, 0(ra)
```

这段代码本身就是第 8 章的练习题：`jal` 会覆盖 `ra`，`t4` 会被被调用者随意使用，
所以两者都必须存栈。**运行时是照着同一份 ABI 手写的**，这也是验证 ABI 的最好方式。

64 位移位按 1 位循环，注意“低位进高位”的顺序：

```asm
__ashldi3:                              // a0/a1 = 值, a2 = 移位量
        beq     a2, zero, .Lshl_done
.Lshl_loop:
        srli    t0, a0, 31              // 低字要移出的那一位
        slli    a1, a1, 1
        or      a1, a1, t0              // 送进高字的低位
        slli    a0, a0, 1
        addi    a2, a2, -1
        bne     a2, zero, .Lshl_loop
```

### 3. `_start`：和 Linux 说两句话

QEMU 的用户模式像一个 Linux 内核：它加载 ELF、跳到 `_start`、提供栈。
我们只需要两个系统调用（`write` = 64，`exit` = 93）：

```asm
_start:
        addi    sp, sp, -16
        sw      ra, 12(sp)
        jal     ra, main
        addi    t0, a0, 0                // main 的返回值就是退出码

        lui     a1, %hi(ok_msg)
        addi    a1, a1, %lo(ok_msg)
        addi    a2, zero, 10             // "rvmin: ok\n"
        beq     t0, zero, .Lprint
        lui     a1, %hi(fail_msg)
        addi    a1, a1, %lo(fail_msg)
        addi    a2, zero, 12
.Lprint:
        addi    a0, zero, 1              // fd = stdout
        addi    a7, zero, 64             // SYS_write
        ecall

        addi    a0, t0, 0
        addi    a7, zero, 93             // SYS_exit
        ecall
```

注意 `_start` 里**没有 `ret`**：它的 `ra` 是内核给的垃圾值，退出必须走 `ecall`。
这里用到的 `lui/addi %hi/%lo` 正是第 7 章的重定位。

### 4. 链接脚本与链接

```ld
ENTRY(_start)
SECTIONS {
  . = 0x10000;
  .text   : { *(.text.start) *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) *(.srodata .srodata.*) }
  .data   : { *(.data .data.*) *(.sdata .sdata.*) }
  .bss    : { *(.bss .bss.*) *(COMMON) }
}
```

`ld.lld -m elf32lriscv -T link.ld -o demo start.o demo.o rt.o`

链接器要做的事恰好是我们在第 7 章留下的三类重定位：`R_RISCV_HI20`、
`R_RISCV_LO12_I`、`R_RISCV_LO12_S`，加上静态调用。没有 `R_RISCV_CALL`、
没有 GOT/PLT、没有动态链接——都是后端刻意不生成的东西。

### 5. 整条链路

`scripts/rvmin-run.sh` 里有意思的一点：**程序由 RVMin 自己的汇编器汇编**。

```bash
clang --target=riscv32-unknown-elf -march=rv32i -mabi=ilp32 -O2 \
      -ffreestanding -fno-builtin -S -emit-llvm demo.c -o demo.ll
llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 demo.ll -o demo.s
llvm-mc -triple=rvmin -filetype=obj demo.s        -o demo.o      # 自己的 MC 层
llvm-mc -triple=rvmin -filetype=obj rvmin-rt.S    -o rt.o
llvm-mc -triple=rvmin -filetype=obj start.S       -o start.o
ld.lld -m elf32lriscv -T link.ld -o demo start.o demo.o rt.o
qemu-riscv32-static ./demo                        # -> rvmin: ok, exit 0
```

这么做有两个额外收益：编译器的输出必须能被自己的汇编器重新接受（第 11 章的
往返门禁在真实程序上再验一遍）；运行时的汇编也必须能被自己的解析器接受。

## 验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop; B=/home/ubuntu/llvm/builds/pr-ctpop

# 完整链路：编译、汇编、链接、运行
"$W/llvm/lib/Target/RVMin/scripts/rvmin-run.sh" "$B/bin"
# rvmin: ok
# PASS: linked 6656 bytes (rv32i), ran under qemu-riscv32-static, all checks green

# 失败路径也要验证：把某个检查改坏，退出码应当非零且打印 fail
#   rvmin: fail
#   exit=1

# 编码门禁（新增 ecall/ebreak 后 85 条）
"$W/llvm/lib/Target/RVMin/scripts/check-rvmin-encodings.sh" "$B/bin"
```

退出码是这套自检的关键：`main` 返回第几个检查失败，`_start` 原样交给 `exit`，
`qemu` 把它变成进程退出码——**没有 printf 也能做断言**。

## 练习

1. 给运行时加 `__muldi3`（提示：把 64 位乘法拆成三个 `__mulsi3`，用
   `slli/srli` 拼高低位；32×32 的高位需要逐位加法或第 13 章的 `mulhu`）。
2. 给 `_start` 加一个 `write(1, ...)` 的 `puts`，让 `demo.c` 能打印整数（需要
   `__udivsi3` 做十进制转换）。
3. 把 `link.ld` 的起始地址改成 `0x80000000`，看看 QEMU 报什么错，理解“用户态
   程序必须加载到用户地址空间”。

## 当前限制（第 12 章）

- 运行时只有 8 个整数 libcall；`__muldi3`/`__divdi3`/`__fixsfsi` 等仍然缺失，
  所以本章的程序不碰 64 位乘除和浮点（要的话得链接真正的 `libgcc`）。
- `ecall` 只用于 `write`/`exit`；没有 `brk`、`mmap`，因此没有 `malloc`。
- 运行时的乘除法是逐位循环（约 32 次迭代），比硬件慢两个数量级；第 13 章的
  M 扩展会把它们换成单条指令。
- 移位量 ≥ 64 的 `i64` 移位没有处理（编译器不会生成）。
- 链接脚本固定 `0x10000`，没有 `PT_GNU_STACK`/`PT_GNU_RELRO` 之类的东西；
  只够跑自检程序。
