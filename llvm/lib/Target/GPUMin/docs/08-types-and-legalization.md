# 第 08 章：类型合法化、窄访存与 i64

## 目标

前七章的值几乎全是 i32。真实 kernel 里到处是 i8、i16、i64 和 i1：
图像是 `uint8`，索引是 `size_t`，谓词是 `bool`。本章把这些类型接进
GM32：

1. `i8/i16` 提升到 `i32` 寄存器，但**内存里保持宽度**：
   `ld.global.u8/s8/u16/s16`、`st.global.b8/b16`；
2. `i1` 在内存里是一字节，只定义 bit 0；store 需要 `SELP` 物化 0/1，
   load 是"字节加载 + `and 1`"；
3. `i64` 拆成两个 `i32`（低半在偏移 0，高半在偏移 4），
   加减法显式生成进位/借位，比较展开成高低字比较；
4. 16 位无符号常量（`0xff`、`0xffff` 这类掩码）可以物化：
   `movi` 符号扩展，再补一对 `shl/shr` 清零高位；
5. `zext(icmp)` 产生的 i32 结果 `setcc` 用 DAG combine 改写成
   `SETP + SELP`；i64 比较在类型合法化之前重写，避开
   `Unexpected setcc expansion!`；
6. 整章通过 `-verify-machineinstrs`，`.o` 与重汇编 `.s` 一致。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch08.sh <llvm-bin-dir>
```

## 背景

### 只有 i32 寄存器，但内存不是 i32

GM32 的通用寄存器是 32 位。LLVM 的类型合法化器对此有标准答案：

| IR 类型 | 合法化动作 | 结果 |
| --- | --- | --- |
| `i1` | 合法（PRED） | 谓词寄存器 |
| `i8` / `i16` | Promote | 值放进 `i32` |
| `i32` | 合法 | `GPR` |
| `i64` | Expand | 两个 `i32` |

值可以提升，**内存宽度不能**。`load i8` 如果选成 `ld.global.b32`，会多
读三个字节：轻则读到相邻字段，重则跨页触发 fault。所以窄访存必须用
真正的窄 opcode，不能"先读字再截断"。

### 为什么 `i1` 在内存里是 1 字节

LLVM 的 `i1` 在内存中占 1 字节（store size 是 8 位），只有 bit 0 有定义。
GM32 没有"谓词 store"，所以：

- `store i1 %p, ptr %q` = `SELP` 把谓词变成 0/1 的 `i32`，再 `st.b8`；
- `load i1` = `ld.u8`，再 `and 1`。

这与 C 的 `_Bool` 表示一致：0 或 1，占一字节。

### 为什么 i64 比较会撞上断言

`i64` 被拆成两个 `i32` 之后，`icmp` 需要先比较高位、相等时再比较低位。
LLVM 的通用展开函数 `ExpandIntOp_SETCC` 有一条断言：

```cpp
assert(NewLHS.getValueType() == N->getValueType(0) &&
       "Unexpected setcc expansion!");
```

它假设 setcc 的结果类型和展开后的操作数同宽（典型 32 位目标：结果也是
i32）。但 GPUMin 的 `i1` 是合法类型，`icmp` 的结果就是 `i1`，于是一比
i64 就 abort。

正确做法不是改这条断言，而是**在类型合法化之前**把 i64 的 setcc 重写成
i32 比较的组合（`PerformDAGCombine`）。这也是本章最大的一块代码。

### 小端：低半在偏移 0

GM32 是小端。`i64` 在内存里的布局：

```text
  偏移 0        偏移 4
+------------+------------+
| 低 32 位   | 高 32 位   |
+------------+------------+
```

所以 `load i64` 的第一条 `ld.b32` 是低半（进入返回寄存器对的第一个，
`r4`），第二条是高半（`r5`）。`store` 的顺序相同。

## 设计

### 类型与内存操作矩阵

| 源码操作 | 合法化后的 DAG | GM32 |
| --- | --- | --- |
| `load i8` + `zext` | `zextloadi8` | `ld.global.u8` |
| `load i8` + `sext` | `sextloadi8` | `ld.global.s8` |
| `load i16` + `zext` | `zextloadi16` | `ld.global.u16` |
| `load i16` + `sext` | `sextloadi16` | `ld.global.s16` |
| `store (trunc i32 to i8)` | `truncstorei8` | `st.global.b8` |
| `store (trunc i32 to i16)` | `truncstorei16` | `st.global.b16` |
| `load i1` | `zextload i1` | `ld.global.u8` + `and 1` |
| `store i1` | 非截断 store（内存更宽） | `SELP` + `st.global.b8` |
| `load i64` | 两个 `load i32` | `ld.global.b32` @0 / @4 |
| `store i64` | 两个 `store i32` | `st.global.b32` @0 / @4 |

### `store i1` 的形状

注意一个容易踩的点：`store i1` 的**内存类型比值类型宽**（8 位 vs 1 位），
所以它在 DAG 里**不是** truncating store。`LegalizeStoreOps` 对非截断
store 只查 `getOperationAction(STORE, i1)`，根本不会走 truncstore 的
动作表。因此必须：

```cpp
setOperationAction(ISD::STORE, MVT::i1, Custom);
```

然后在 `LowerOperation` 里把它改成 `zext(i1 -> i32)` + `truncstore i32 -> i8`。
依赖 `setTruncStoreAction(i1, i8, ...)` 是不够的——那条路径永远不会被走到。

### i64 的加减与比较

加法（`ExpandIntRes_ADDSUB` 的形状）：

```text
lo  = loA + loB
carry = (lo <u loA)          ; SETP.ULT，无符号回绕
hi  = hiA + hiB + carry      ; carry 先物化成 0/1
```

减法：

```text
lo    = loA - loB
borrow = (loA <u loB)        ; SETP.ULT
hi    = hiA - hiB - borrow
```

比较（本章的 combine）：

```text
ult(a,b)  = (hiA <u hiB) || (hiA == hiB && loA <u loB)
slt(a,b)  = (hiA <s hiB) || (hiA == hiB && loA <u loB)
eq(a,b)   = (loA == loB) && (hiA == hiB)
```

谓词寄存器没有 AND/OR 指令，所以布尔组合要先把谓词物化成 0/1 的 i32，
用 ALU 组合，再 `SETP.NE 0` 变回谓词。代价是多几条 `SELP`，但语义清晰。

### 常量物化

| 常量范围 | 序列 | 指令数 |
| --- | --- | --- |
| `[-32768, 32767]` | `movi rd, imm` | 1 |
| `[32768, 65535]` | `movi rd, imm-65536; shl rd,16; shr rd,16` | 3 + 1（16 的常量） |
| 其他 32 位 | `movi rd,hi; movi rt,16; shl rd,rd,rt; movi rt2,lo; shl/shr rt2; or rd,rd,rt2` | 7 |

第二行是本章新增的：`movi` 的立即数是 simm16，`65535` 会被汇编器拒绝；
但 `-1` 的 16 位补码就是 `0xffff`。所以先 `movi rd, -1`（寄存器里是
`0xffffffff`），再 `shl 16` / `shr 16`，得到 `0x0000ffff`。

这也解释了为什么汇编里会看到 `movi r2, -1` 而不是 `movi r2, 65535`：
打印的是规范化的 simm16。

## 实现

| 层 | 文件 | 内容 |
| --- | --- | --- |
| 合法化 | `GPUMinISelLowering.cpp` | 窄 load/store 动作、i1 load/store、i64 比较 combine、i32 setcc combine |
| ISel | `GPUMinISelDAGToDAG.cpp` | `SIGN_EXTEND_INREG`、无符号 16 位常量、`ANY_EXTEND i1` |
| TableGen | `GPUMinInstrInfo.td` | `zextloadi8/s8/u16/s16`、`truncstorei8/i16` 模式 |
| 门禁 | `scripts/check-ch08.sh` | 4 组检查 |

### 合法化动作

```cpp
for (MVT VT : {MVT::i8, MVT::i16}) {
  setLoadExtAction(ISD::EXTLOAD, MVT::i32, VT, Legal);
  setLoadExtAction(ISD::ZEXTLOAD, MVT::i32, VT, Legal);
  setLoadExtAction(ISD::SEXTLOAD, MVT::i32, VT, Legal);
  setTruncStoreAction(MVT::i32, VT, Legal);
}

setOperationAction(ISD::STORE, MVT::i1, Custom);
setTruncStoreAction(MVT::i1, MVT::i8, Custom);
setLoadExtAction(ISD::EXTLOAD, MVT::i32, MVT::i1, Custom);
setLoadExtAction(ISD::ZEXTLOAD, MVT::i32, MVT::i1, Custom);
setLoadExtAction(ISD::SEXTLOAD, MVT::i32, MVT::i1, Custom);
```

### `zext(icmp)` 的 combine

```cpp
if (N->getOpcode() == ISD::SETCC && N->getValueType(0) == MVT::i32) {
  SDValue Cmp = DAG.getSetCC(DL, MVT::i1, N->getOperand(0),
                             N->getOperand(1), CC);
  return DAG.getSelect(DL, MVT::i32, Cmp,
                       DAG.getConstant(1, DL, MVT::i32),
                       DAG.getConstant(0, DL, MVT::i32));
}
```

这里必须用 `select` 而不是 `zext`：`(and (zext a), (zext b))` 会被
DAGCombiner 折回 i1 的 `and`，而谓词寄存器没有布尔 ALU。用 `select 1/0`
就不会触发那条折叠。

### i64 比较：先交换，再拆半

```cpp
if (N->getOpcode() == ISD::SETCC &&
    N->getOperand(0).getValueType() == MVT::i64) {
  bool Swap = false;
  switch (CC) {
  case ISD::SETUGT: CC = ISD::SETULT; Swap = true; break;
  case ISD::SETUGE: CC = ISD::SETULE; Swap = true; break;
  case ISD::SETGT:  CC = ISD::SETLT;  Swap = true; break;
  case ISD::SETGE:  CC = ISD::SETLE;  Swap = true; break;
  default: break;
  }
  if (Swap) std::swap(L, R);        // 必须在取半之前
  SDValue LLo = Half(L, 0), LHi = Half(L, 1);
  ...
}
```

"先交换、再取半"是容易写错的地方：如果先取 `LLo/RLo` 再交换 `L/R`，
greater 系列会悄悄变成用错误操作数比较。

### 寄存器里的 i16 符号扩展

`sext i16` 直接来自 load 时会折进 `ld.global.s16`；如果值已经在寄存器里
（函数参数、ALU 结果），DAG 里是 `sign_extend_inreg i32:i16`，ISel 手工
展开成移位对：

```asm
movi r1, 16
shl r2, r4, r1
sar r4, r2, r1
```

### 无符号 16 位常量

```cpp
if (isUInt<16>(C->getZExtValue())) {
  int32_t SImm = static_cast<int16_t>(V);   // 65535 -> -1
  SDValue Imm = CurDAG->getTargetConstant(static_cast<uint32_t>(SImm),
                                          DL, MVT::i32);
  // movi rd, SImm; movi rt, 16; shl rd, rd, rt; shr rd, rd, rt
}
```

`SImm` 的强制转换不是装饰：直接把 `65535` 交给 MOVI，asn 打印出
`movi r2, 65535`，汇编器会报 `invalid operand for instruction`，于是
"codegen 的 `.o` 与重汇编的 `.s` 一致"这条门禁立刻失败。

## 代码走读

### `load i64` / `store i64`

```asm
ld64:
	ld.global.b32 r1, 0(r4)     # 低半
	ld.global.b32 r5, 4(r4)     # 高半 -> 返回寄存器对的第二个
	mov r4, r1
	ret

st64:
	st.global.b32 r5, 0(r4)     # 低半
	st.global.b32 r6, 4(r4)     # 高半
	ret
```

### `add64`

```asm
	add r1, r4, r6              # lo = loA + loB
	setp.ult p0, r1, r4         # carry = lo <u loA
	movi r2, 0
	movi r3, 1
	selp r2, p0, r3, r2         # carry 变成 0/1
	add r3, r5, r7              # hiA + hiB
	add r5, r3, r2              # + carry
	mov r4, r1
	ret
```

### `cmp64`（`icmp ult i64`）

```asm
	setp.ult p0, r4, r6         # loA <u loB
	setp.eq p1, r5, r7          # hiA == hiB
	setp.ult p2, r5, r7         # hiA <u hiB
	movi r1, 0
	movi r2, 1
	selp r3, p2, r2, r1         # hiA <u hiB  -> 0/1
	selp r4, p0, r2, r1         # loA <u loB  -> 0/1
	selp r1, p1, r2, r1         # hiA == hiB  -> 0/1
	and r1, r1, r4              # (hi==) && (lo<)
	or r4, r3, r1               # || (hi<)
	ret
```

### `store i1`

```asm
	setp.lt p0, r5, r6
	movi r1, 0
	movi r2, 1
	selp r1, p0, r2, r1         # 谓词 -> 0/1
	st.global.b8 r1, 0(r4)      # 只写一个字节
	ret
```

### `load i1`

```asm
	ld.global.u8 r1, 0(r4)      # 读包含 bit 0 的那个字节
	movi r2, 1
	and r4, r1, r2              # 只留 bit 0
	ret
```

### `shl i16`：掩码常量

```asm
	movi r1, 16
	movi r2, -1                 # 0xffffffff
	shl r2, r2, r1              # 0xffff0000
	shr r1, r2, r1              # 0x0000ffff
	and r1, r5, r1              # 截断到 16 位
	shl r4, r4, r1
	ret
```

## 验证

```text
$ llvm/lib/Target/GPUMin/scripts/check-ch08.sh /home/ubuntu/llvm/builds/pr-ctpop/bin
PASS 1/4: narrow loads select u8/s8/u16/s16
PASS 2/4: narrow stores keep width and i1 is a byte
PASS 3/4: i64 is little-endian halves with carry and compare
PASS 4/4: codegen object and reassembled .s are identical
OK: chapter 08 gate passed
```

四组分别覆盖：

1. 四种窄加载、`sext(load i16)` 的折叠、寄存器内 i16 符号扩展的移位对；
2. `st.b8/b16`、`store i1` 的 `SELP + st.b8`、`load i1` 的 `and 1`；
3. `ld64/st64` 的 0/4 偏移、`add64/sub64` 的 `setp.ult`、`cmp64` 的
   `setp.eq`；
4. `-verify-machineinstrs` + `.o`/`.s` 往返一致。

手工复核：

```sh
llc -march=gpumin -verify-machineinstrs /tmp/types.ll -o /tmp/types.s
llvm-mc -triple=gpumin -filetype=obj /tmp/types.s -o /tmp/types-mc.o
llvm-objdump -d -r /tmp/types.o
```

## 常见坑

1. **窄 load 不能用 `b32` + 截断**。多读三个字节会改变 volatile 语义，
   也可能跨页 fault。宽度必须进 opcode。
2. **`store i1` 不是 truncating store**。内存类型比值类型宽，通用合法化器
   只看值类型，会直接放过；必须给 `STORE i1` 设 Custom。
3. **`i1` 的 load/store 都要物化**。谓词文件没有"存一个 bit"的指令，
   也没有"从字节读谓词"的指令。
4. **SETCC 的动作按操作数类型查**。`LegalizeDAG` 对 SETCC/SELECT_CC 用的是
   `getOperationAction(SETCC, OpVT)`，不是结果类型。给 i32 设 Custom 会把
   i1 结果的 setcc 一起卷进来。
5. **i64 setcc 会触发 `Unexpected setcc expansion!`**。因为 i1 是合法类型，
   通用展开的断言不成立。务必在类型合法化之前的 combine 里重写。
6. **布尔 AND/OR 没有指令**。谓词要先用 `SELP` 变成 0/1，ALU 组合，再
   `SETP.NE 0`。不要指望 `ISD::AND i1` 能选出来。
7. **`(and (zext a), (zext b))` 会被折回 i1 AND**。用 `select 1/0` 物化，
   可以绕开这条 combine。
8. **`select(p, 1, 0)` 会被折成 `ANY_EXTEND`**。ISel 里 `ANY_EXTEND i1`
   要和 `ZERO_EXTEND i1` 一样处理（都是 0/1，高位的 "any" 取 0 合法）。
9. **`movi` 打印无符号值会汇编失败**。`65535` 必须规范化成 `-1`；
   低位相同，移位对随后清零高位。
10. **greater 系列要先交换再拆半**。先拆半再交换会让 `ugt/sgt` 用错
    操作数，而且没有编译错误，只有错误结果。
11. **i64 参数/返回值"免费"支持**。`CCIfType<[i32]>` 会匹配展开后的两个
    半字，所以 i64 走 `r4/r5` 对；但 i64 的**栈参数**仍然不支持。
12. **完整 32 位常量要自己拼**。本章的 `Select()` 只处理
    `[-32768, 32767]` 和 `[0, 65535]`；任意 32 位常量需要高低半拼接
    （`demos/02-alu-and-constants.ll` 就是这条路径的样例）。拼接时最
    容易错的是低位：`movi` 符号扩展，直接 `or` 会把高 16 位污染成 1，
    必须再 `shl/shr` 一次。

## 练习

1. **优化 32 位常量**：现在固定 7 条指令。低位 `lo` 落在
   `[-32768, 32767]` 时可以省掉那对 `shl/shr`；高位为 0 时可以只用
   无符号路径。写出更短的序列并补测试。
2. **i64 乘法/除法**：本章只有加减。乘法可以拆成 32×32→64 的
   `mul`/`mulh`（需要新指令），除法建议先走 libcall。
3. **i64 比较直接进分支**：现在先物化 0/1 再比较；可以让
   `br (icmp i64)` 直接消费谓词组合，省掉 `SELP`。
4. **窄参数 ABI**：给 i8/i16 参数补 signext/zeroext 属性，保证被调用者
   看到的高位是规范的。
5. **i1 的寄存器表示**：如果以后要做 `activemask`（第 09 章），
   需要把 32 个 lane 的谓词打包成一个 i32；这是下一章的前置。
6. **`i128`**：把本章的拆半规则递归一层，观察 legalizer 的
   `ExpandIntRes_*` 如何复用。
7. **未对齐窄访存**：`ld.u16` 在奇数地址上的行为目前由硬件/模拟器决定；
   可以加一个 `allowsMemoryAccess` 的检查或拆成两次 `u8`。

## 下一章

第 09 章：**特殊寄存器与 intrinsic**。本章让类型在寄存器和内存之间
正确搬运；下一章引入 `laneid/warpid/blockid/tid/activemask` 这些
SIMT 只读状态，以及它们在 LLVM 里的入口——target intrinsic。届时
`i1` 的谓词文件会第一次承担"每 lane 一位"的位图语义。
