# 第 10 章：RV32I 的缺口——乘除、位运算内建与软浮点

## 目标

到第 9 章为止，RVMin 能编译整数、指针、数组、结构体、递归、64 位算术。但有三类
东西在 RV32I 里**一条指令都没有**：

| 缺口 | 属于哪个扩展 | 本章的解法 |
| --- | --- | --- |
| 乘法、除法、取模 | M 扩展 | 调用编译器运行时（`__mulsi3` 等） |
| `cpop`/`clz`/`ctz`/`bswap` | Zbb / Zbkb | 展开成 RV32I 序列 |
| 任何浮点运算 | F / D 扩展 | 软浮点，全部变成 libcall |

```c
int muls(int a, int b) { return a * b; }        /* → __mulsi3  */
int divs(int a, int b) { return a / b; }        /* → __divsi3  */
unsigned remu(unsigned a, unsigned b) { return a % b; }  /* → __umodsi3 */
long long mul64(long long a, long long b) { return a * b; }  /* → __muldi3 */

int popc(unsigned x) { return __builtin_popcount(x); }   /* 内联成算术序列 */
unsigned bsw(unsigned x) { return __builtin_bswap32(x); } /* 内联成移位对 */

float fadd(float a, float b) { return a + b; }   /* → __addsf3 */
double dmul(double a, double b) { return a * b; } /* → __muldf3 */
int fcmp(float a, float b) { return a < b; }     /* → __ltsf2 + slti */
int f2i(float f) { return (int)f; }              /* → __fixsfsi */
float i2f(int i) { return (float)i; }            /* → __floatsisf */
```

完成本章后：

1. `i32`/`i64` 的 `mul`/`div`/`rem` 全部可用（有符号与无符号）；
2. 2 的幂的乘法、常量除法仍然展开成移位；
3. `__builtin_popcount`/`clz`/`ctz`/`bswap32` 内联成 RV32I 序列，不产生调用；
4. `float`/`double` 的加减乘除、比较、与整数互转全部可用（软浮点）；
5. 所有 libcall 都是普通函数调用，复用第 8 章的调用约定与第 9 章的类型合法化。

## 代码地图

| 文件 | 本章改动 |
| --- | --- |
| `RVMinISelLowering.cpp` | `MUL`/`DIV`/`REM`/`*_LOHI`/`*DIVREM`/位运算/`ROT*` 的 `Expand` 动作；软浮点说明 |
| `llvm/test/CodeGen/RVMin/libcalls.ll` | 本章 FileCheck（libcall 名字、内联展开、重定位） |

这一章几乎没有新代码：**所有工作都是声明“我不会，请走通用机制”**。这正是 LLVM
的合法化框架的价值——缺指令不等于缺功能。

## 关键实现

### 1. `Expand` 的两条不同路径

给一个 opcode 设置 `Expand`，合法化器会先试 `ExpandNode()`（内联展开），失败后再走
`ConvertNodeToLibcall()`（生成库调用）。同一个语义，两条路径：

```
LegalizeOp(Node):
  case Expand:
    if (ExpandNode(Node))     // ① 内联：移位/加法/比较拼出来
      return;
    ConvertNodeToLibcall(Node); // ② 展开不了就发一个调用
```

所以“能不能内联”不是我们决定的，而是通用展开器判断的：

| 运算 | `ExpandNode` 能内联吗 | 结果 |
| --- | --- | --- |
| `mul x, 4` | 能（移位） | `slli` |
| `mul x, y` | 不能（没有通用位运算序列） | `__mulsi3` |
| `sdiv x, 8` | 能 | 符号修正 + `srai` |
| `sdiv x, y` | 不能 | `__divsi3` |
| `ctpop x` | 能（经典位技巧） | 约 12 条算术指令 |
| `bswap x` | 组合子先改写成 rotate | 移位 + `or` |

### 2. 乘除法：一行动作换一个库函数

```cpp
for (MVT VT : {MVT::i32}) {
  setOperationAction(ISD::MUL,  VT, Expand);   // __mulsi3
  setOperationAction(ISD::SDIV, VT, Expand);   // __divsi3
  setOperationAction(ISD::UDIV, VT, Expand);   // __udivsi3
  setOperationAction(ISD::SREM, VT, Expand);   // __modsi3
  setOperationAction(ISD::UREM, VT, Expand);   // __umodsi3
}
```

`i64` 不需要单独写：第 9 章把 `i64` 拆成两个 `i32`，64 位乘除自然落到
`MUL_I64`/`SDIV_I64` 这些 libcall 上，符号是 `__muldi3`/`__divdi3`。

生成结果（`-O1`）：

```
mul:
        addi    sp, sp, -16
        sw      ra, 12(sp)
        lui     t0, %hi(__mulsi3)
        addi    t0, t0, %lo(__mulsi3)
        jalr    ra, 0(t0)
        lw      ra, 12(sp)
        addi    sp, sp, 16
        jalr    zero, 0(ra)
```

这些符号名不是我们选的：`RuntimeLibcalls.td` 里 `def __mulsi3 : RuntimeLibcallImpl<MUL_I32>;`
定义了“`MUL_I32` 这个抽象需求在默认 ABI 下叫 `__mulsi3`”。换 ABI（比如 ARM 的
AAPCS）会自动换成 `__aeabi_*`，后端代码一行都不用改。

### 3. 一个必须踩的坑：复合节点默认是 `Legal`

第一版只写了 `MUL`/`SDIV`/`SDIV` 三条动作，结果：

```
LLVM ERROR: Cannot select: t9: i32,i32 = smul_lohi t4, t2
LLVM ERROR: Cannot select: t9: i32,i32 = sdivrem t2, t4
```

原因：`TargetLowering` 的每个 opcode 默认动作都是 **`Legal`**。通用展开器在展开
`MUL` 时，会先看“两结果乘法 `SMUL_LOHI` 合不合法”，而这个节点默认合法，于是它
直接把 `MUL` 换成了 `SMUL_LOHI`，根本没机会走到 libcall；指令选择阶段才发现没有
这条指令。

所以复合节点必须显式关掉：

```cpp
setOperationAction(ISD::MULHS,    VT, Expand);
setOperationAction(ISD::MULHU,    VT, Expand);
setOperationAction(ISD::SMUL_LOHI, VT, Expand);  // 否则 MUL 展开成两结果乘法
setOperationAction(ISD::UMUL_LOHI, VT, Expand);
setOperationAction(ISD::SDIVREM,  VT, Expand);   // 否则 DIV 展开成商余两结果
setOperationAction(ISD::UDIVREM,  VT, Expand);
```

规律可以总结成一句话：**通用展开器会优先使用“更原始”的节点，这些节点的默认
`Legal` 是一个陷阱；每章加动作时，要顺着展开器的偏好链条一起关掉。**

### 4. 位运算内建：内联比调用更好

`__builtin_popcount` 在 IR 里是 `llvm.ctpop.i32`。设置 `Expand` 后，`ExpandNode`
用经典的位技巧把它内联展开（约 12 条指令，无调用、无栈帧）：

```
popc:
        lui     a1, 349525
        addi    a1, a1, 1365      # 0x55555555
        srli    a2, a0, 1
        and     a1, a2, a1
        sub     a0, a0, a1        # x - ((x >> 1) & 0x55555555)
        ...                       # 5 轮之后得到 popcount
```

`bswap` 更有意思：DAG 组合子先把它改写成 `rotr`（半字交换 + 字节 rotate），
所以还必须让 `ROTL`/`ROTR` 可展开，否则会看到：

```
LLVM ERROR: Cannot select: t8: i32 = rotr t2, Constant:i32<16>
```

```
bsw:
        lui     a1, 16
        addi    a1, a1, -256      # 0x00FF00FF
        srli    a2, a0, 8
        and     a2, a2, a1
        srli    a3, a0, 24
        or      a2, a2, a3
        ...
```

这一节的教学点：**“缺指令”有三种处理方式——内联展开、libcall、或者自己加指令**。
选择哪一种取决于代价模型和运行时是否可依赖。

### 5. 软浮点：什么都不用做

这是本章最反直觉的部分。我们没有 `f32`/`f64` 寄存器类，也没写任何浮点代码，但
下面这些都能编译：

```
fadd:   lui t0, %hi(__addsf3)   ...
dmul:   lui t0, %hi(__muldf3)   ...
fcmp:   lui t0, %hi(__ltsf2)    ...
f2i:    lui t0, %hi(__fixsfsi)  ...
i2f:    lui t0, %hi(__floatsisf) ...
```

机制是**类型软化（type softening）**：没有为 `f32` 注册寄存器类，类型合法化器就
认为“这个类型在这台机器上不合法”，于是把每个 `f32` 值软化成一个 `i32`、每个
`f64` 软化成一对 `i32`，并把所有浮点运算改写成对应的 libcall。这些 libcall 的
参数/返回值恰好都是整数寄存器里的整数，于是第 8 章的调用约定直接复用。

`fcmp` 的细节值得一看：

```
fcmp:
        ...
        jalr    ra, 0(t0)        # __ltsf2(a, b) 返回 <0 / 0 / >0
        slti    a0, a0, 0        # 三元结果 → 布尔
```

因为 libcall 返回的是三路比较结果（类似 `strcmp`），还需要一条 `slti` 才能变成
`i1`。这就是“软浮点不是免费的”：一次比较 = 一次调用 + 一次比较。

### 6. 这些符号从哪来

后端只负责**发出**调用，不负责提供实现。要真正链接出可执行文件，需要：

- `libgcc` / `compiler-rt` 提供的 `__mulsi3`、`__divsi3`、`__muldi3`、
  `__ashldi3`、`__addsf3`、`__muldf3`、`__ltsf2`、`__fixsfsi`、`__floatsisf` …；
- 或者自己写一个最小的 `rvmin-rt.c`（第 12 章可做）。

对教学后端来说这是一个很好的边界：**指令集最小，但 ABI 与运行时是完整的**。
`llvm-readelf -r` 能看到这些调用对应的重定位：

```
R_RISCV_HI20     __mulsi3 + 0
R_RISCV_LO12_I   __mulsi3 + 0
```

## 验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop; B=/home/ubuntu/llvm/builds/pr-ctpop

# 1) 本章 FileCheck：libcall 名字、内联展开、重定位
"$B/bin/llvm-lit" -sv "$W/llvm/test/CodeGen/RVMin/libcalls.ll"

# 2) 全部 RVMin 测试
"$B/bin/llvm-lit" -s "$W/llvm/test/CodeGen/RVMin"          # 8/8

# 3) 编码门禁（本章不新增指令，应保持 83 条）
"$W/llvm/lib/Target/RVMin/scripts/check-rvmin-encodings.sh" "$B/bin"

# 4) 每个测试 × -O0..-O3 × 机器码校验
for f in "$W"/llvm/test/CodeGen/RVMin/*.ll; do
  for O in 0 1 2 3; do
    "$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O$O \
      -verify-machineinstrs "$f" -o /dev/null
  done
done

# 5) clang 端到端 + 汇编往返（乘除、位内建、软浮点混合）
"$B/bin/clang" --target=riscv32-unknown-elf -march=rv32i -mabi=ilp32 \
  -O1 -S -emit-llvm ops.c -o ops.ll
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O1 ops.ll -filetype=obj -o a.o
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O1 ops.ll -o a.s
"$B/bin/llvm-mc" -triple=riscv32-unknown-elf -filetype=obj a.s -o b.o
# 抽出 .text 后 cmp：628 字节一致
```

本次结果：`8/8` lit 通过，`-O0`…`-O3` 全部通过 `-verify-machineinstrs`，
clang 端到端口语料 `.text` **628 字节**与 `llvm-mc` 重汇编逐字节相同，
符号表里出现 `__mulsi3`/`__divsi3`/`__udivsi3`/`__modsi3`/`__umodsi3`/
`__muldi3`/`__divdi3`/`__addsf3`/`__muldf3`/`__ltsf2`/`__fixsfsi`/`__floatsisf`/
`__extendsfdf2`/`__truncdfsf2`。

## 练习

1. 把 `x * 3` 也变成移位+加法：提示 `ExpandNode` 调用
   `expandMUL(..., MulExpansionKind::OnlyLegalOrCustom)`，而常量路径要求半字类型
   （`i16`）合法。想办法在不实现 `i16` 的情况下让它内联（或证明为什么不行）。
2. 给 `CTPOP` 加一条 `Custom` 降低，用 `mul` 之外的位技巧（提示：`x & (x-1)` 循环），
   比较代码大小。
3. 用 `llvm-mc` 写 `__mulsi3` 的 RV32I 实现（移位加法循环），和第 12 章的运行时
   连起来。

## 当前限制（第 10 章）

- 常量乘法只对 2 的幂内联成移位；`x * 3`、`x * 100` 仍走 `__mulsi3`
  （正确但慢，见练习 1）。
- 除法/取模是纯 libcall，没有“除以常量”的强度削减（除 2 的幂除外）。
- 软浮点的所有语义（舍入模式、异常标志、`-ffast-math` 的快速路径）都依赖运行时；
  后端不区分 `strictfp`。
- `fp128`/`x86_fp80` 等扩展格式没有对应的 libcall 名字（不在本章范围）。
- 依赖 `libgcc`/`compiler-rt`；没有它们时链接会失败，但对象文件与汇编完全正确。
