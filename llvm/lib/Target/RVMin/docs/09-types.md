# 第 9 章：整数类型合法化（i1 / i8 / i16 / i64）

## 目标

RV32I 的寄存器是 32 位的，而 C 里有 `_Bool`、`char`、`short`、`long long`。
第 8 章之前我们只处理 `i32`：一旦源码里出现窄类型，指令选择就会崩：

```
LLVM ERROR: Cannot select: t9: i32 = sign_extend_inreg t2, ValueType:ch:i16
LLVM ERROR: Cannot select: t5: i32 = sdiv t2, t4      （第 10 章）
```

本章把这些类型接到已有的 RV32I 指令上：

```c
char gc;  short gs;  long long gk;  _Bool flag;

int add_char(char a, char b) { return a + b; }        /* i8  → 提升成 i32 */
int add_short(short a, short b) { return a + b; }
unsigned char uc(unsigned char a, unsigned char b) { return a + b; }
int ld(char *p) { return (signed char)p[0] + (unsigned char)p[1]; }
void st(short *p, int v) { p[0] = v; }                /* 截断存储 */

long long add64(long long a, long long b) { return a + b; }
int lt64(long long a, long long b) { return a < b; }
long long shl64(long long a, int n) { return a << n; } /* → __ashldi3 */

struct S { char c; short s; int i; long long q; };
int fields(struct S *p) { p->c = 1; p->s = 2; p->i = 3; p->q = 4; ... }
```

完成本章后：

1. `i8`/`i16` 的 load 用 `LB`/`LBU`/`LH`/`LHU`，store 用 `SB`/`SH`；
2. `sext iN (trunc i32 x)`（`sign_extend_inreg`）用两条移位实现；
3. `_Bool` 在内存里占一个字节，与 `char` 共用同一套访存模式；
4. `i64` 被拆成两个 `i32`：加法带上 `sltu` 进位，比较按高/低半字组合；
5. 变长 `i64` 移位展开成 `__ashldi3`/`__lshrdi3`/`__ashrdi3` 调用；
6. 窄类型作为返回值时按 ABI 做符号/零扩展；
7. `struct` 拷贝经 `memcpy` 库函数工作（复用第 8 章的调用支持）。

## 代码地图

| 文件 | 本章改动 |
| --- | --- |
| `RVMinInstrInfo.td` | 窄 load/store 的 TableGen 模式（含 `addr + offset` 折叠） |
| `RVMinISelDAGToDAG.cpp` | `getLoadOpcode()`/`getStoreOpcode()`：按内存类型选宽窄 |
| `RVMinISelLowering.cpp` | `SIGN_EXTEND_INREG`、`i1` 访存、`*_PARTS` 的合法化动作 |
| `scripts/rvmin-global-corpus.s` | 窄访存/移位对/进位链的字节门禁 |
| `llvm/test/CodeGen/RVMin/types.ll` | 本章 FileCheck（含 `-O0`、重定位） |

## 关键实现

### 1. 类型合法化在做什么

SelectionDAG 走到指令选择之前，会先把 IR 类型“合法化”到目标支持的集合。
RV32I 只有 `i32`，于是：

| 源类型 | 合法化方式 | 结果 |
| --- | --- | --- |
| `i1` | Promote | 值在寄存器里是 `i32`（0/1） |
| `i8` | Promote | 值在寄存器里是 `i32`，内存里仍是 1 字节 |
| `i16` | Promote | 值在寄存器里是 `i32`，内存里仍是 2 字节 |
| `i64` | Expand（Split） | 拆成 `(lo, hi)` 两个 `i32` |

关键点：**“值类型”被提升，但“内存类型”不变**。所以 `char` 的 load 在
DAG 里是“结果为 `i32`、内存类型为 `i8`、带符号扩展”的一个节点：

```
t10: i32,ch = load<(load (s8) from %ir.p), sext from i8> t0, t2
```

这正是 `LB` 的形状，所以它必须由后端负责，而不是先把 `char` 当成 `i32` 读。

合法化动作默认是 `Legal`。默认值意味着“后端声称能选出来”，如果其实选不出来，
错误直到 `Select()` 里才出现，就是上面那种 `Cannot select`。所以每章我们做的
事情，本质是给一张 `(opcode, VT) → action` 表补条目。

### 2. 窄访存：内存类型决定指令宽度

TableGen 模式按“内存类型 + 扩展方式”区分，共 6 种 load 形态：

| DAG 节点 | 语义 | 指令 |
| --- | --- | --- |
| `sextloadi8` | 读 1 字节，符号扩展 | `LB` |
| `zextloadi8` | 读 1 字节，零扩展 | `LBU` |
| `extloadi8` | 读 1 字节，随便扩展 | `LBU`（任一种都合法，选零扩展） |
| `sextloadi16` | 读 2 字节，符号扩展 | `LH` |
| `zextloadi16` | 读 2 字节，零扩展 | `LHU` |
| `extloadi16` | 读 2 字节，随便扩展 | `LHU` |

存储方向是“截断存储”：值寄存器是 `i32`，只写低 1/2 字节。

```tablegen
def : Pat<(i32 (sextloadi8 GPR:$addr)), (LB GPR:$addr, 0)>;
def : Pat<(i32 (sextloadi8 (add GPR:$addr, simm12:$off))),
          (LB GPR:$addr, simm12:$off)>;
def : Pat<(truncstorei16 GPR:$val, GPR:$addr), (SH GPR:$val, GPR:$addr, 0)>;
```

`(add GPR:$addr, simm12:$off)` 这一组是必要的：第 6 章把 GEP 折叠成
`base + 常数`，而 `p->c`、`p[2]` 这类访问全都长这样。

栈上的窄对象走的是另一条路——`RVMinISelDAGToDAG.cpp` 里的 frame-index
特化，它绕过了 TableGen（因为 frame index 不是普通寄存器操作数）：

```cpp
unsigned getLoadOpcode(const LoadSDNode *LD) {
  switch (LD->getMemoryVT().getSimpleVT().SimpleTy) {
  case MVT::i8:  return LD->getExtensionType() == ISD::ZEXTLOAD ? RVMin::LBU
                                                               : RVMin::LB;
  case MVT::i16: return LD->getExtensionType() == ISD::ZEXTLOAD ? RVMin::LHU
                                                               : RVMin::LH;
  case MVT::i32: return RVMin::LW;
  default:       return 0;   // 交给 TableGen 模式，别在这里吞掉
  }
}
```

返回 `0` 表示“我不认识”，于是继续走通用路径。这是让手写选择器与 TableGen
共存的关键：不要用 `llvm_unreachable`，否则以后加类型要改两处。

### 3. `sign_extend_inreg`：用两条移位做符号扩展

`char a, b; return a + b;` 在 IR 里是：

```llvm
%c = add i8 %a, %b
%r = sext i8 %c to i32
```

提升成 `i32` 后，加法自然变成 32 位加法，但结果必须按 8 位截断再符号扩展，
否则高 24 位是垃圾。DAG 用 `sign_extend_inreg` 表达这个语义：

```
t9: i32 = sign_extend_inreg t2, ValueType:ch:i8
```

RV32I 没有这种指令，但两条移位就够了：

```
slli  a0, a0, 24        # 把低 8 位移到最高位
srai  a0, a0, 24        # 算术右移，用符号位填满高 24 位
```

对 16 位就是 `slli 16` / `srai 16`。我们让合法化器自动展开，指令选择里一行
特例都不用写：

```cpp
// 注意：查询用的是“内层类型”，不是结果类型
for (MVT VT : {MVT::i1, MVT::i8, MVT::i16})
  setOperationAction(ISD::SIGN_EXTEND_INREG, VT, Expand);
```

**这是一个真实的坑**。第一版写的是：

```cpp
setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i32, Expand);  // 完全无效
```

因为 `LegalizeDAG.cpp` 里查表时用的是 `VTSDNode` 的类型：

```cpp
case ISD::SIGN_EXTEND_INREG: {
  EVT InnerType = cast<VTSDNode>(Node->getOperand(1))->getVT();
  Action = TLI.getOperationAction(Node->getOpcode(), InnerType);
  break;
}
```

也就是说，`i32` 只是“结果的类型”，合法化器根本没问它。这类“动作注册在哪个
VT 上”的问题不看源码是猜不出来的，这也是本章最值得记住的一条经验。

展开后的效果（`sext_inreg` 用例，`-O1`）：

```
sext_inreg:
        slli    a0, a0, 16
        srai    a0, a0, 16
        jalr    zero, 0(ra)
```

### 4. `i1`：内存里就是一个字节

`_Bool` 的处理和 `char` 完全一样，只差一个动作：默认的 `i1` load 是
`load<(load (s8)...), zext from i1>`，它的内存类型是 `i1`，我们的
`LB/LBU/SB` 模式匹配不到。两种办法：

- 给 `i1` 也写一套 `LB/LBU/SB` 模式（多 4 条重复模式）；
- 让合法化器把内存类型从 `i1` 提升到 `i8`（推荐）。

```cpp
// i1 在内存里就是一个字节，把内存类型提升成 i8 复用窄访存模式
setLoadExtAction(ISD::SEXTLOAD, MVT::i32, MVT::i1, Promote);
setLoadExtAction(ISD::ZEXTLOAD, MVT::i32, MVT::i1, Promote);
setLoadExtAction(ISD::EXTLOAD,  MVT::i32, MVT::i1, Promote);
setTruncStoreAction(MVT::i32, MVT::i1, Promote);
```

`Promote` 之后，`_Bool` 的读写就和 `char` 一模一样：

```
load_bool:
        lbu     a0, 0(a0)
        jalr    zero, 0(ra)

store_bool:
        andi    a1, a1, 1
        sb      a1, 0(a0)
```

`andi` 是 `truncstorei1` 语义带来的：IR 保证值是 0/1，但 DAG 仍会显式截一位。

### 5. `i64`：拆成两个 `i32`

`splitting` 之后所有 64 位运算都变成成对的操作。加法的进位不是 `addc`，
而是一次无符号比较：

```
add_i64:
        add     a1, a1, a3      # hi += hi
        add     a2, a0, a2      # lo = lo1 + lo2
        sltu    a0, a2, a0      # lo 结果比任一加数小 → 进位
        add     a1, a1, a0      # hi += carry
        addi    a0, a2, 0
```

64 位比较不能简单拆成两次 32 位比较，因为“低位大小”只在“高位相等”时才有
意义。合法化器会生成组合逻辑：

```
lt_i64:                         # (a.hi < b.hi) | (a.hi == b.hi & a.lo <u b.lo)
        xor     a4, a1, a3
        slt     a1, a1, a3
        sltu    a0, a0, a2
        xor     a0, a0, a1
        sltiu   a2, a4, 1
        and     a0, a0, a2
        xor     a0, a0, a1
```

这里没有新增指令——`slt/sltu/sltiu/xor/and` 第 4 章就已经有了，本章的价值在于
“证明这套指令足够”。

### 6. 64 位移位：`SHL_PARTS` 与编译器运行时

一个 32 位寄存器装不下 0..63 的移位量，DAG 于是把 64 位移位表示为三段
（`SHL_PARTS`/`SRL_PARTS`/`SRA_PARTS`），每段用“移位量 <32 / ==32 / >32”
分支拼接。手写这套拼接没有教学价值，标准做法是交给编译器运行时：

```cpp
setOperationAction(ISD::SHL_PARTS, MVT::i32, Expand);
setOperationAction(ISD::SRL_PARTS, MVT::i32, Expand);
setOperationAction(ISD::SRA_PARTS, MVT::i32, Expand);
```

（`*_PARTS` 的动作同样注册在“结果类型”`i32` 上——每个元素是 `i32`。）

展开结果是一个普通函数调用，而第 8 章已经能发直接调用了：

```
shl_i64:
        addi    sp, sp, -16
        sw      ra, 12(sp)
        lui     t0, %hi(__ashldi3)
        addi    t0, t0, %lo(__ashldi3)
        jalr    ra, 0(t0)
        lw      ra, 12(sp)
        addi    sp, sp, 16
        jalr    zero, 0(ra)
```

对应重定位（`llvm-readelf -r`）：

```
R_RISCV_HI20     __ashldi3 + 0
R_RISCV_LO12_I   __ashldi3 + 0
```

注意常数移位不会被展开成库调用：`x >> 3` 在 DAG 里就是 `srai/srli` 的拼接，
只有**变长**的 64 位移位才需要运行时。这也是“小后端也能跑真实代码”的原因：
libcall 把复杂度外置了，代价是必须能链接 `compiler-rt`（或等价的
`libgcc`）。`__ashldi3`、`__lshrdi3`、`__ashrdi3` 就是这里需要的三个符号。

### 7. 窄类型的 ABI 边界

ILP32 规定：窄于 32 位的**参数和返回值**按符号性扩展成 32 位后再放进 `a0`
（调用方负责参数，被调用方负责返回值）。我们不需要为参数写代码——clang 在
IR 里已经带上了 `sext`/`zext`；返回值则因为 IR 里是 `ret i8`，会被本章的
`sign_extend_inreg` 顺手处理掉：

```c
signed char rc(signed char x) { return x + 1; }
unsigned char ru(unsigned char x) { return x + 1; }
```

```
rc:                             ru:
        slli    a0, a0, 24              addi    a0, a0, 1
        lui     a1, 4096                andi    a0, a0, 255
        addi    a1, a1, 0               jalr    zero, 0(ra)
        add     a0, a0, a1
        srai    a0, a0, 24
        jalr    zero, 0(ra)
```

`lui a1, 4096; addi a1, a1, 0` 就是常数 `4096` 的两段式物化（第 4 章）。

### 8. `memcpy`：结构体拷贝变成库调用

结构体按值拷贝在 IR 里是 `memcpy` intrinsic，SelectionDAG 把它变成一次库调用。
因为第 8 章已经有了调用+栈参数+`ra` 保存，这条路不用改一行代码：

```c
struct T { char c[10]; };
void cp(struct T *a, struct T *b) { *a = *b; }
```

```
cp:
        addi    sp, sp, -16
        sw      ra, 12(sp)
        addi    a2, zero, 10
        lui     t0, %hi(memcpy)
        addi    t0, t0, %lo(memcpy)
        jalr    ra, 0(t0)
        lw      ra, 12(sp)
        addi    sp, sp, 16
        jalr    zero, 0(ra)
```

这也说明“后端最小”和“能编译真实 C”并不矛盾：**缺失的指令用运行时补**。

## 常见坑

- **`SIGN_EXTEND_INREG` 按内层类型注册**：legalizer 查询的是
  `VTSDNode` 里的那个 VT（i1/i8/i16），给 `MVT::i32` 设 `Expand` 完全无效。
- **`i1` 的内存类型要 `Promote` 到 i8**：否则 TableGen 要求一条“i1 load”，
  而 RV32I 只有字节/半字/字访问。`setLoadExtAction(..., MVT::i1, Promote)`
  把它接回 `LB/LBU`。
- **`i64` 的移位是 `SHL_PARTS`/`SRL_PARTS`/`SRA_PARTS`**：不 `Expand` 就会
  停在选择器里；展开后是 `__ashldi3`/`__lshrdi3`/`__ashrdi3` 调用。
- **`i64` 的 mul/div 是两结果节点**：`SMUL_LOHI`/`UMUL_LOHI`/`SDIVREM`/
  `UDIVREM` 默认 `Legal`，必须显式 `Expand`（第 10 章踩过同一坑）。
- **窄参数的高位是调用方的责任**：ABI 要求调用者把 `char`/`short` 扩展好，
  函数入口不再扩展；手写 IR 传脏高位会算出错误结果。

## 验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop; B=/home/ubuntu/llvm/builds/pr-ctpop

# 1) 本章 FileCheck（窄访存、sext_inreg、i64、_Bool）
"$B/bin/llvm-lit" -sv "$W/llvm/test/CodeGen/RVMin/types.ll"

# 2) 全部 RVMin 测试
"$B/bin/llvm-lit" -s "$W/llvm/test/CodeGen/RVMin"          # 7/7

# 3) 字节门禁：新增的窄访存/移位对/进位链与参考汇编器逐字节一致
"$W/llvm/lib/Target/RVMin/scripts/check-rvmin-encodings.sh" "$B/bin"

# 4) 每个测试 × -O0..-O3 × 机器码校验
for f in "$W"/llvm/test/CodeGen/RVMin/*.ll; do
  for O in 0 1 2 3; do
    "$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O$O \
      -verify-machineinstrs "$f" -o /dev/null
  done
done

# 5) clang 端到端 + 汇编往返：llc 生成的 .s 用 llvm-mc 重汇编，
#    .text 必须逐字节相同
"$B/bin/clang" --target=riscv32-unknown-elf -march=rv32i -mabi=ilp32 \
  -O1 -S -emit-llvm types.c -o types.ll
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O1 types.ll -filetype=obj -o a.o
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O1 types.ll -o a.s
"$B/bin/llvm-mc" -triple=riscv32-unknown-elf -filetype=obj a.s -o b.o
# objcopy 抽出 .text 后 cmp：244 字节一致
```

本次结果：`7/7` lit 通过，`PASS: 83 instructions, .text identical`，
`-O0`…`-O3` 全部通过 `-verify-machineinstrs`，clang 端到端 `.text` 244 字节
与 `llvm-mc` 重汇编结果逐字节相同。

## 练习

1. 把 `sext_inreg` 的展开改成手写 `SELECT_CC`（`x << 16 < 0 ? -1 : 0xFFFF`），
   观察指令数和 `-O2` 下的差异。
2. 去掉 `i1` 的 `Promote`，只补 TableGen 模式，看看会多出几条重复模式；再想想
   为什么 `setTruncStoreAction` 不能靠模式解决。
3. 用 `llc -debug-only=legalize-types` 观察一个 `i64` 函数被拆分的全过程。

## 当前限制（第 9 章）

- `i8`/`i16` 的乘除取模仍然不可用：它们先被提升成 `i32`，然后撞上第 10 章的
  乘除法缺口。
- `i64` 的 `mul`/`div`/`rem` 同样等第 10 章（`__muldi3`、`__divdi3` 等）。
- 变长 `i64` 移位依赖 `__ashldi3`/`__lshrdi3`/`__ashrdi3`；结构体拷贝依赖
  `memcpy`。这些符号必须由运行时库提供，后端本身不生成它们的实现。
- 浮点类型完全不支持（RV32I 没有 F 扩展，第 10 章只做软浮点判断）。
- 窄类型的**参数**依赖调用方按 ABI 扩展；我们不在函数入口重新扩展，因此
  手写 IR（不是 clang 产出）传进来脏的高位会得到错误结果。
- 帧内偏移仍限制在 `[-2048, 2047]`，`i64` 数组会更快撞到这个上限。
