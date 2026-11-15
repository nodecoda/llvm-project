# 第 13 章：可选的 M 扩展与优化空间

## 目标

第 10 章把 RV32I 的三个缺口（乘、除、取模）交给了 libcall：

```asm
        lui     t0, %hi(__mulsi3)
        addi    t0, t0, %lo(__mulsi3)
        jalr    ra, 0(t0)
```

正确，但一次乘法要付出一次函数调用、一次栈帧和 32 次循环。RISC-V 的 **M 扩展**
（`RV32IM` 里的 `M`）就是把这八个操作做成单条 `OP` 指令。本章把它作为**可选特性**
加进后端，同时保持“不打开就是纯 RV32I”：

```console
$ llc -march=rvmin -O1 muldiv.ll            # 默认：libcall
$ llc -march=rvmin -O1 -mattr=+m muldiv.ll  # 打开 M：单条指令
$ llc -march=rvmin -O1 -mcpu=generic-rv32-m muldiv.ll
```

完成本章后：

1. `-mattr=+m` / `-mcpu=generic-rv32-m` 两种写法都能打开 M 扩展；
2. 八个指令（`mul/mulh/mulhsu/mulhu/div/divu/rem/remu`）在 TableGen 里由
   `Predicates` 保护：不打开 M 时既选不出来，也汇编不了；
3. 汇编器对“指令需要 M”给出诊断，而不是静默丢弃（这是本章顺手修掉的真 bug）；
4. 编码门禁多一份 M 语料，端到端门禁多一遍 `+m` 扫描，QEMU 里同一程序两种
   构建输出一致，`+m` 版本不再引用任何乘除 libcall。

## 代码地图

| 文件 | 内容 |
| --- | --- |
| `RVMin.td` | `FeatureStdExtM`、`HasStdExtM`、`generic-rv32-m` 处理器 |
| `RVMinSubtarget.h` | `HasStdExtM` 位与 `hasStdExtM()` 查询 |
| `RVMinInstrInfo.td` | 八条 M 指令 + 七个 TableGen 选择模式 |
| `RVMinISelLowering.cpp` | 打开 M 时不再把乘除降级为 libcall |
| `AsmParser/RVMinAsmParser.cpp` | `Match_MissingFeature` 诊断 |
| `scripts/rvmin-m-encoding-corpus.s` | 12 条 M 指令的编码语料 |
| `scripts/check-rvmin-encodings.sh` | M 语料的逐字节比对 + 反例检查 |
| `llvm/test/CodeGen/RVMin/m-ext.ll` | `+m` 与默认模式的 lit 测试 |
| `scripts/rvmin-e2e.sh` | `-mattr=+m` 扫描（7 程序 × 4 优化级 × 2 特性） |
| `scripts/rvmin-run.sh` | 同一程序编译两次并在 QEMU 里比对输出 |

## 关键实现

### 1. 一个特性三个东西：`SubtargetFeature`、`Predicate`、`Processor`

TableGen 里描述“可选特性”只需要三小段：

```tablegen
def FeatureStdExtM : SubtargetFeature<"m", "HasStdExtM", "true",
                                      "Integer multiplication and division">;

def HasStdExtM : Predicate<"Subtarget->hasStdExtM()">,
                 AssemblerPredicate<(all_of FeatureStdExtM),
                                    "'M' (Integer Multiplication and Division)">;

def : RVMinProc<"generic-rv32-m", [FeatureStdExtM]>;
```

三者的分工是本章最重要的概念：

- **`SubtargetFeature`** 生成 `RVMin::FeatureStdExtM` 这个位号，并让
  `ParseSubtargetFeatures` 在特性串里看到 `+m` 时把 `HasStdExtM` 置为 `true`。
  名字（`"m"`）就是命令行里的 `+m`。
- **`Predicate`** 是给指令选择用的 C++ 表达式；**`AssemblerPredicate`** 是给
  汇编器匹配器用的特性位。`HasStdExtM` 同时携带两者，所以同一个 `Predicates`
  列表既挡住指令选择，也挡住汇编解析——一条声明，两处生效。
- **`Processor`** 把特性打包成一个 `-mcpu` 名字。`generic-rv32` 是空列表，
  `generic-rv32-m` 就是 `generic-rv32 + M`。

`RVMinSubtarget` 里只多了两行——一个字段和一个查询：

```cpp
  bool HasStdExtM = false;
public:
  bool hasStdExtM() const { return HasStdExtM; }
```

字段由生成的 `ParseSubtargetFeatures` 写入（`Tab` 里的 `"HasStdExtM"` 就是字段名），
查询函数被 `Predicate` 和手写 lowering 共用。

> **坑**：`RVMin::FeatureStdExtM` 这个 enum 由
> `MCTargetDesc/RVMinMCTargetDesc.h` 里的 `GET_SUBTARGETINFO_ENUM` 生成。如果
> `RVMinSubtarget.h` 再 include 一次这个块，会得到 “conflicts with a previous
> declaration”；不 include 它则 `ParseSubtargetFeatures` 找不到 enum。正确做法是
> **在子目标头文件里 include `RVMinMCTargetDesc.h`**，只让一处定义 enum。

### 2. 八条指令与七个模式

M 扩展的全部指令都是 R 型，`funct7 = 0b0000001`，只差 `funct3`：

```tablegen
let Predicates = [HasStdExtM] in {

def MUL    : RVMinRInst<0b0000001, 0b000, OPC_OP, (outs GPR:$rd),
                       (ins GPR:$rs1, GPR:$rs2), "mul", "$rd, $rs1, $rs2">;
def MULH   : RVMinRInst<0b0000001, 0b001, OPC_OP, ...>;
def MULHSU : RVMinRInst<0b0000001, 0b010, OPC_OP, ...>;
def MULHU  : RVMinRInst<0b0000001, 0b011, OPC_OP, ...>;
def DIV    : RVMinRInst<0b0000001, 0b100, OPC_OP, ...>;
def DIVU   : RVMinRInst<0b0000001, 0b101, OPC_OP, ...>;
def REM    : RVMinRInst<0b0000001, 0b110, OPC_OP, ...>;
def REMU   : RVMinRInst<0b0000001, 0b111, OPC_OP, ...>;

def : Pat<(mul   GPR:$a, GPR:$b), (MUL   GPR:$a, GPR:$b)>;
def : Pat<(sdiv  GPR:$a, GPR:$b), (DIV   GPR:$a, GPR:$b)>;
...
} // Predicates = [HasStdExtM]
```

`RVMinRInst` 是第 1 章就写好的 R 型模板，所以新增指令只是填四个字段。选择模式
覆盖七个 DAG 节点：`mul/sdiv/udiv/srem/urem/mulhs/mulhu`。`mulhsu` 没有对应的
IR 惯用法（需要混合符号扩展），只保留汇编能力。

编码与参考汇编器一致（`scripts/rvmin-m-encoding-corpus.s`）：

```
        mul  a0, a1, a2                 # encoding: [0x33,0x85,0xc5,0x02]
        div  a0, a1, a2                 # encoding: [0x33,0xc5,0xc5,0x02]
        rem  a0, a1, a2                 # encoding: [0x33,0xe5,0xc5,0x02]
        remu a0, a1, a2                 # encoding: [0x33,0xf5,0xc5,0x02]
```

### 3. lowering：不能“看见 M 就 break”

第 10 章的降级写在构造函数的一个循环里：

```cpp
  for (MVT VT : {MVT::i32}) {
    setOperationAction(ISD::MUL,  VT, Expand);
    ...
    setOperationAction(ISD::CTLZ, VT, Expand);   // 循环里还有别的动作
  }
```

最自然的改法是“有 M 就跳过这段”，但 `break` 会把循环后面**位运算内建**的
`Expand` 也一起跳过——`clz`/`ctz`/`bswap` 立刻变成不可选择的节点，`-O0` 下
`arith.c` 直接 `Cannot select: ctlz`。所以只能条件包裹：

```cpp
    // 两结果节点没有单条指令可用，两种模式下都必须是 Expand
    setOperationAction(ISD::SMUL_LOHI, VT, Expand);
    setOperationAction(ISD::UMUL_LOHI, VT, Expand);
    setOperationAction(ISD::SDIVREM, VT, Expand);
    setOperationAction(ISD::UDIVREM, VT, Expand);

    if (!STI.hasStdExtM()) {
      setOperationAction(ISD::MUL,  VT, Expand);
      setOperationAction(ISD::MULHS, VT, Expand);
      ...
    }
```

第二条经验关于那四个**两结果**节点。第 10 章把它们设成 `Expand` 是为了避免
“默认 Legal 却选不出来”的 `SMUL_LOHI`；打开 M 之后它们仍然是 `Expand`，只是
展开的终点从 libcall 变成了 `mul`+`mulh`。`i64` 乘法正是这么来的：

```console
$ cat w.c
long long f(long long a, long long b) { return a * b; }
$ llc -march=rvmin -mattr=+m -O1 w.ll -o -
        mul     a1, a2, a1              # 交叉项：a.lo * b.hi
        mulhu   a4, a2, a0              # 需要 32×32 的高半
        add     a1, a4, a1
        mul     a3, a3, a0              # 另一项：a.hi * b.lo
        add     a1, a1, a3
        mul     a0, a2, a0              # 低半：a.lo * b.lo
```

没有 `mulhu`（纯 RV32I）时，同样这段代码会变成对 `__muldi3` 的调用。

### 4. 汇编器：给出诊断，而不是静默丢弃

加完指令后先做反例测试，发现了一个**真 bug**：

```console
$ llvm-mc -triple=rvmin mul.s            # 没有 +m
$ echo $?
0
```

`mul` 被静默接受了，`.text` 是空的。原因在
`RVMinAsmParser::matchAndEmitInstruction`：生成匹配器在特性不满足时返回
`Match_MissingFeature`，而 switch 只处理了 `Match_Success`/`Match_MnemonicFail`/
`Match_InvalidOperand`，`default` 分支直接 `return true`——既没有诊断，也没有把
“缺失特性”翻译成人话。

修法分两步。先取带 `MissingFeatures` 输出的重载：

```cpp
  FeatureBitset MissingFeatures;
  unsigned MatchResult = MatchInstructionImpl(Operands, Inst, ErrorInfo,
                                              MissingFeatures, MatchingInlineAsm);
```

再处理这个返回值。特性名由 TableGen 从 `AssemblerPredicate` 生成：

```cpp
  case Match_MissingFeature: {
    bool First = true;
    std::string Msg = "instruction requires the following:";
    for (unsigned Feature : MissingFeatures) {
      Msg += First ? " " : ", ";
      Msg += getSubtargetFeatureName(Feature);
      First = false;
    }
    return Error(IDLoc, Msg);
  }
```

`getSubtargetFeatureName` 在生成文件末尾，定义在 `#ifdef GET_SUBTARGET_FEATURE_NAME`
里，所以要（a）在底部 `#define GET_SUBTARGET_FEATURE_NAME`，（b）在文件顶部加
一个前向声明（它是 `static`）。现在的诊断：

```console
$ llvm-mc -triple=rvmin mul.s
mul.s:1:1: error: instruction requires the following: 'M' (Integer Multiplication and Division)
mul a0, a1, a2
^
```

门禁把这条反例也钉住了：没有 `+m` 时 `llvm-mc` 必须以非零码退出，且 stderr
里必须出现 `requires the following: 'M'`。

### 5. 三处门禁一起扩展

- **编码**：新增 `rvmin-m-encoding-corpus.s`（12 条，覆盖八个 mnemonic 和多个
  寄存器别名），`rvmin` 与 `riscv32` 都带 `-mattr=+m` 逐字节比对；
- **端到端**：`rvmin-e2e.sh` 把特性集变成外层循环（空 / `+m`），llc 和
  `llvm-mc` 同时带 `-mattr`，于是 7 个程序 × 4 个优化级 × 2 个特性 = 56 个对象，
  每个 `.text` 都要和重汇编逐字节相等；
- **运行**：`rvmin-run.sh` 把同一份 `demo.c` 编译两次，分别链接运行，要求输出一致，
  并且 `+m` 版本的对象**不再引用**五个乘除取模 libcall。

`demo.c` 为此改成用 `volatile` 全局变量传操作数——否则 `1000 * 37` 在 clang 里
直接折叠成常量，libcall 和 `mul` 都不会出现，门禁就是空的。改完之后：

```console
$ llvm-readelf -r demo.o        # 默认 RV32I 构建
__ashldi3  __divsi3  __modsi3  __mulsi3  __udivsi3  __umodsi3

$ llvm-readelf -r demo-m.o      # +m 构建
__ashldi3
```

64 位变长移位仍然是 libcall：`<<`/`>>` 在 `long long` 上不属于 M 扩展。

## 验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop; B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B" llc clang llvm-mc llvm-objdump llvm-readelf llvm-objcopy FileCheck

# 1) 两种打开方式
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O1 -mattr=+m t.ll -o -
"$B/bin/llc" -march=rvmin -mtriple=riscv32-unknown-elf -O1 -mcpu=generic-rv32-m t.ll -o -

# 2) 编码门禁（85 条 RV32I + 12 条 M + 缺失特性反例）
"$W/llvm/lib/Target/RVMin/scripts/check-rvmin-encodings.sh" "$B/bin"

# 3) lit（含 m-ext.ll）
"$B/bin/llvm-lit" -s "$W/llvm/test/CodeGen/RVMin"

# 4) 端到端 + 运行
"$W/llvm/lib/Target/RVMin/scripts/rvmin-e2e.sh" "$B/bin"
"$W/llvm/lib/Target/RVMin/scripts/rvmin-run.sh" "$B/bin"
```

`rvmin-run.sh` 的预期输出：

```console
rv32i: rvmin: ok
rv32i+m: rvmin: ok
PASS: linked 6656 bytes (rv32i) and 6592 bytes (rv32i+m), both ran under qemu-riscv32-static
      with identical output; +m dropped the mul/div/rem libcalls entirely
```

## 练习

1. 加 `Zmmul`（只有乘法、没有除法的子集）：复制 `FeatureStdExtM`，把四个
   `mul*` 的 `Predicates` 改成 `[HasStdExtM, HasStdExtZmmul]` 的两个谓词组合，
   体会 `all_of`/`any_of`。
2. 给 `rvmin-rt.S` 的 `__mulsi3` 加一个 `#ifdef`，用 `mul` 实现它，然后用
   `-mattr=+m` 跑第 12 章的链接脚本——运行时也可以享受硬件乘法。
3. 让 `-mcpu=generic-rv32-m` 自动打开 `+m` 而不是依赖 `-mattr`，用
   `llvm-mc -mcpu=generic-rv32-m` 验证汇编器也吃这一套。

## 当前限制（第 13 章）

- M 扩展是可选的，但**没有** `A`/`C`/`F`/`D`/`Zbb` 等其它扩展；软浮点仍是
  libcall，原子操作不存在。
- `mulhsu` 只支持汇编，没有 IR 惯用法映射；`i64` 的乘除仍走 `__muldi3` 系列
  或展开（取决于优化级）。
- 没有指令调度模型（`NoItineraries`），`-mattr=+m` 只影响指令选择和编码，
  不影响顺序；`-O2` 的寄存器分配仍然是最朴素的实现。
- 帧大于 2 KiB 仍会报错（缺 `RegScavenger` 物化地址）；没有尾调用优化，
  `return f(x);` 仍是 call + ret。
- 常量乘法只有 2 的幂会被折叠（`x * 8` → `slli`），`x * 3`、`x * 7` 这类
  仍是 `mul`（或默认模式下的 `__mulsi3`）；`x * 10` 之类的 strength reduction
  留给下一步。
- `switch` 仍然降级为比较链：`BR_JT`/`BRIND` 没有实现，所以没有跳转表，
  也就没有 `jalr` 间接跳转。
- 汇编器只接受本章给出的 RV32I + M 指令。其它扩展的 mnemonic 会报
  “invalid instruction mnemonic”，即使打开了对应特性。
- 没有 `.option arch` / `.attribute arch`：特性只在命令行（`-mattr`/`-mcpu`）
  上生效，不能在一个汇编文件里切换。
