# 27. arith 方言：它是什么，以及怎么练到精通

> 上游基线：`mlir/include/mlir/Dialect/Arith/`（2127 行 `ArithOps.td`）、
> `mlir/lib/Dialect/Arith/`（`ArithOps.cpp` 3706 行，56 个 `fold`、10 个 `verify`）、
> `mlir/test/Dialect/Arith/`（32 个 `.mlir`）。
> 配套：26 章（它的文本只有 8 类语句）、13 章（`assemblyFormat` 子语言）。
> 本文不是 API 手册，是**学习路线图**：每阶段都有可检验的通关判据。

## 0. 一句话

**arith 是 MLIR 的数值基座**：标量 / 向量 / 张量上的基本算术、比较、转换和常量，
**没有内存、没有控制流、没有超越函数**，所有 op 都是 `Pure`（无副作用、可任意
重排/消除）。它是几乎所有前端与高层方言最终的数值落点，也是整个 IR 里
`fold` 产生属性后重新物化成值的入口（`ArithDialect.cpp:55` 的
`materializeConstant`）。

## 1. 它在生态里的位置

| 关系 | 谁 | 说明 |
| --- | --- | --- |
| 上游 | `tosa` / `stablehlo` / `linalg` / `tensor` / `vector` / `scf` | 高层语义最终都落到 arith 的数值原语上（`TosaToArith` 就是一个独立转换目录） |
| 下游 | `llvm` / `spirv` / `amdgpu` / `arm_sme` / `emitc` | `ArithToLLVM`、`ArithToSPIRV`、`ArithToAMDGPU`、`ArithToArmSME`、`ArithToEmitC` |
| 依赖 | `builtin`（类型）、`ub`（poison） | arith 自己几乎不依赖别的方言 |
| 被依赖的接口 | `InferIntRangeInterface`、`InferIntDivisibilityOpInterface`、`ValueBoundsOpInterface`、`BufferizableOpInterface` | 分析/优化基础设施直接读 arith 的 op |

关键事实：**`arith` 的 op 没有 region**（`ArithOps.td` 里 `hasRegion` 出现 0 次），
所以它永远是叶子语句——这也是 26 章能用一套平语句文法描述它的原因。

## 2. 数据模型与边界

### 2.1 类型

| 类型 | 说明 |
| --- | --- |
| `iN` | **signless** 整数。MLIR 的类型层没有 signed/unsigned，符号性由 **op 名**决定（`divsi` vs `divui`）。位宽上限 2^24−1（`invalid.mlir:120` 的 `i16777216` 负例） |
| `fN` | 浮点，含低精度家族：`f8E4M3FN`、`f8E4M3FNUZ`、`f8E5M2`、`f8E5M2FNUZ`、`f8E8M0FNU`（缩放指数）、`f4E2M1FN`（fp4） |
| `index` | 目标相关的指针宽度整数；与 `iN` 的互转要显式 `index_cast` / `index_castui` |
| 容器 | `vector<...>` / `tensor<...>`；绝大多数 arith op 支持（`SameOperandsAndResultShape`） |

### 2.2 什么不属于 arith（也是"新 op 该不该进来"的判据）

| 不属于 | 归属 | 判据 |
| --- | --- | --- |
| 内存读写、指针 | `memref` / `llvm` | arith 全部 `Pure`，不碰内存 |
| 分支、循环 | `scf` / `cf` | arith 无 region |
| `sin`/`cos`/`exp`/`sqrt` | `math` | arith 只做"基本算术" |
| 复数运算 | `complex` | |
| 形状/广播/切片 | `tensor` / `vector` | arith 只做逐元素 |
| 有符号/无符号**类型** | 不存在 | 符号性在 op 名里 |

判断一个候选 op 能不能进 arith，用这四条：**纯值语义、逐元素、无 region、无超越**。
四条都满足才进得来；缺一条就该去 `math`/`vector`/`scf`。

## 3. 56 个 op 的族谱（语义要点）

26 章按**文本形状**分成 8 类；这里按**语义**分组，只标容易错的地方。

| 族 | 数量 | 代表 | 语义要点 |
| --- | --- | --- | --- |
| 整数二元 | 16 | `addi subi muli divsi divui ceildivsi ceildivui floordivsi remsi remui andi ori xori shli shrsi shrui` | 除法三个方向：`divsi` 向零、`floordivsi` 向下、`ceildivsi` 向上；`remsi` 符号随被除数。除零与 `INT_MIN / -1` 是 **UB**（`ArithOps.td:642`） |
| 浮点二元 | 11 | `addf subf mulf divf remf maximumf maxnumf maximumnumf minimumf minnumf minimumnumf` | min/max 有**三种 NaN 语义**，见 §3.1 |
| 整数 min/max | 4 | `maxsi maxui minsi minui` | `Total` 族：全定义域有定义，不会 poison |
| 一元 | 2 | `negf flush_denormals` | 只有浮点取负；整数取负用 `subi 0, x` |
| 比较 | 2 | `cmpi cmpf` | CmpI 10 个谓词、CmpF 16 个（含 `true`/`false`） |
| 选择 | 1 | `select` | 条件 `i1` 或同形状的 `vector`/`tensor`；shaped 条件时文本要额外打条件类型 |
| 常量 | 1 | `constant` | 值是 typed attribute，**类型必须与结果一致**（`invalid.mlir:77`）；BoolAttr 自带 `i1` 所以文本可省类型 |
| 转换 | 13 | `extf extsi extui truncf trunci fptosi fptoui sitofp uitofp index_cast index_castui bitcast convertf` | 符号性在 op 名；`bitcast` 只能同宽重解释 |
| 多结果 | 4 | `addui_extended subui_extended mulsi_extended mului_extended` | 第二个结果是溢出位（`i1`），**不 poison** |
| FP8 缩放 | 2 | `scaling_extf scaling_truncf` | OCP MXFP：`scale` 是 E8M0 块缩放（`ArithOps.td:1504`） |

### 3.1 最常错的一组：浮点 min/max

| op | 一个操作数是 NaN | `-0.0` vs `+0.0` | 标准 |
| --- | --- | --- | --- |
| `maximumf` | 结果 NaN（传播） | `+0.0` 更大 | IEEE 754-2019 `maximum` |
| `maxnumf` | 返回另一个数（忽略 NaN） | 结果任选 | IEEE 754-2008 `maxNum` |
| `maximumnumf` | 返回另一个数（忽略 NaN） | `+0.0` 更大 | IEEE 754-2019 `maximumNumber` |

（`minimum*` 对称；源码：`ArithOps.td:1161`/`:1181`/`:1202`。）

### 3.2 溢出与 poison

| 写法 | 溢出时 |
| --- | --- |
| `addi/subi/muli ... overflow<nsw, nuw>` | 结果 **poison**（不是回绕） |
| `addui_extended` 等 | 返回 `(值, 溢出位)`，**不 poison** |
| `divsi ... exact` | 不整除时结果 **poison**（不是"精确除法"的断言） |
| `fptosi`/`fptoui` 越界 | poison |

## 4. 三组属性

| 属性 | 取值 | 谁用 |
| --- | --- | --- |
| `Arith_FastMathAttr` | `none fast nnan ninf nsz arcp contract afn reassoc`（9 个拼写；`none`/`fast` 是空集/全集，中间 7 个可组合） | 浮点 op |
| `Arith_IntegerOverflowFlags` | `nsw` / `nuw`（可组合） | 整数 op、`trunci` |
| `Arith_RoundingModeAttr` | `to_nearest_even to_nearest_away upward downward toward_zero` | `truncf`、`convertf`、`scaling_truncf` |

`nsz` 有个细节：`maximumnumf(+0.0, -0.0)` 在 `nsz` 下可任选，但同号零必须保留
符号（`ArithOps.td:1202` 的说明）——`mulf-nsz-signed-zero.mlir` 就是这条的测试。

## 5. 变换与降级地图

| 目录 | 内容 |
| --- | --- |
| `lib/Dialect/Arith/IR/ArithCanonicalization.td` | 声明式 canonicalization 模式 |
| `lib/Dialect/Arith/IR/ArithOps.cpp` | 56 个 `fold`（**每个 op 都有 folder**）、10 个 `verify`、`SelectOp` 的手写 parse/print |
| `lib/Dialect/Arith/IR/InferIntRangeInterfaceImpls.cpp` | 28 个 `inferResultRanges`（整数区间推导） |
| `lib/Dialect/Arith/IR/InferIntDivisibilityOpInterfaceImpl.cpp` | 整除性推导 |
| `lib/Dialect/Arith/IR/ValueBoundsOpInterfaceImpl.cpp` | 值域约束（`ValueBoundsOpInterface`） |
| `Transforms/ExpandOps.cpp` | `--arith-expand`：min/max、bf16/fp8 的 ext/trunc 展开 |
| `Transforms/EmulateWideInt.cpp` | `--arith-emulate-wide-int`：2N 位整数用 N 位模拟 |
| `Transforms/EmulateNarrowType.cpp` | 窄类型模拟的转换器与 arith 模式（`populateArithNarrowTypeEmulationPatterns`）；生产入口在 memref 侧 |
| `Transforms/EmulateUnsupportedFloats.cpp` | `--arith-emulate-unsupported-floats`：不支持的浮点用 extf/truncf 模拟 |
| `Transforms/UnsignedWhenEquivalent.cpp` | `--arith-unsigned-when-equivalent`：可证等价时 signed → unsigned |
| `Transforms/IntRangeOptimizations.cpp` | `--int-range-optimizations` / `--arith-int-range-narrowing` |
| `Conversion/ArithToLLVM` | 逐 op 的 `ConvertOpToLLVM`（`BitcastOpLowering` 等），溢出旗标 → `nsw/nuw`，fastmath → fast flags |
| 其余转换 | `ArithToSPIRV`、`ArithToAMDGPU`、`ArithToArmSME`、`ArithToEmitC`、`ArithAttrToLLVM`、`ArithAndMathToAPFloat` |

## 6. 学习计划（6 阶段，全职 2 周 / 业余 4–6 周）

每个阶段四件事：**目标 → 必读 → 动手 → 通关判据**。判据不过不要进下一阶段。

### S0 定位（0.5 天）

- **目标**：说清 arith 是什么、不是什么；能读、能写、能往返。
- **必读**：本文 §0–§2；`ArithBase.td:15`（方言定义）；26 章 §1–§2。
- **动手**：写 10 条 arith 语句，跑
  `mlir-opt --mlir-print-op-generic` 对照 custom 与 generic 两种文本。
- **判据**：能解释为什么 `arith.addi` 不能带 region；能默写 8 个语义族。

### S1 类型与值域（1–2 天）

- **目标**：把"signless"讲透；把 poison/UB 和"未定义"分清。
- **必读**：`invalid.mlir:120`（位宽上限）、`:77`（常量类型必须匹配）；
  §3.2 的 poison 表。
- **动手**：给每一类"溢出/越界/除零"各写一条负例，跑 `mlir-opt` 确认报错文本。
- **判据**：能回答——`arith.muli` 带 `nsw` 溢出后，值是什么？为什么不是回绕？

### S2 op 语义（3–5 天）

- **目标**：56 个 op 的边界行为全部过一遍，重点是 §3.1 的三种 NaN 与除法四兄弟。
- **必读**：`ArithOps.td` 每个 def 的 description（这是权威语义，不是文档站）；
  `ops.mlir` 与 `invalid.mlir`。
- **动手**：为三个 min/max 各写一对 NaN/±0 用例，用 `--canonicalize` 观察
  `maxnumf` 不能被折叠成 `maximumf` 的原因。
- **判据**：§8 的 18 道题答对 15 道以上。

### S3 文本层（2–3 天）

- **目标**：custom 与 generic 两条通道都熟；知道哪些东西格式串不说。
- **必读**：13 章 §5；26 章 §4（18 条事实）与 §10.1（L2 的四个缺口）。
- **动手**：用 `tools/arith-check.py` 和 `mlir-opt` 对同一段文本做差分：
  一边用本仓库的参考解析器，一边用真 parser，比较"接受/拒绝/边界"。
- **判据**：能说出 `arith.constant 42 : i32` 里的 `: i32` 由谁打印（属性，不是格式）。

### S4 变换（5–7 天）

- **目标**：能预测 `--canonicalize` 做什么；能读一个 fold/pattern 并判断合法性。
- **必读**：`ArithCanonicalization.td`；`ArithOps.cpp` 挑 5 个 `fold` 精读；
  `IntRangeOptimizations.cpp`；`ExpandOps.cpp` 的 min/max 展开。
- **动手**：在 `int-range-opts.mlir` / `int-range-narrowing.mlir` 上跑
  `--int-range-optimizations`、`--arith-int-range-narrowing`，逐条解释每个变化。
- **判据**：给一段 `arith` 代码，能手工写出 canonicalize 后的结果并说明依据。

### S5 降级与生态（3–5 天）

- **目标**：知道每个 op 落到 LLVM 是什么；知道哪些 arith 语义在 LLVM 里没有对应。
- **必读**：`ArithToLLVM.cpp`（自顶向下读 `populateArithToLLVMConversionPatterns`）；
  `ArithDialect.cpp:55`（`materializeConstant` 与 `ub.poison`）；
  `BufferizableOpInterfaceImpl.cpp`。
- **动手**：做一张 `arith op → LLVM dialect op → LLVM IR` 对照表，
  标出"语义有损"的三处（除零 UB、`remsi` 符号、fp8）。
- **判据**：能解释为什么 `arith.divsi` 不是无条件 speculatable
  （`floordivsi-speculatability.mlir`）。

### S6 精通校验（持续）

- 给一个 arith op 加 folder 或 verifier，跑通对应 lit 测试；
- 读一个 arith 相关 PR，指出它的测试覆盖了哪条语义；
- 独立提一个修复（测试先行）。

## 7. 精通的四级判据

| 级别 | 你能做什么 | 检验方式 |
| --- | --- | --- |
| L1 会用 | 写 IR、跑 pass、看懂报错 | 给需求写一段 arith 程序并优化 |
| L2 懂语义 | 预测 fold/canonicalize 结果；解释 poison/UB/NaN | §8 自测 + 手算 canonicalize |
| L3 能改 | 加 folder/verifier/interface；修一个 bug | 一个带测试的小 patch |
| L4 能设计 | 判断新 op 该不该进 arith；判断某变换是否合法 | 用 §2.2 四条判据评审一个提案 |

多数人停在 L2。**L3 的分水岭是"能写出让 lit 失败的测试"**；L4 的分水岭是
"能说出这个 op 为什么不能放进 `math`"。

## 8. 自测 18 题（答案在题后）

1. `arith.addi` 能用于 `index` 吗？
2. `divsi(-7, 2)` 与 `floordivsi(-7, 2)` 分别是多少？
3. `remsi(-7, 2)` 的符号？
4. `muli` 带 `nsw` 溢出后，结果是回绕值还是 poison？
5. `addui_extended` 溢出时返回什么？
6. `maximumf(NaN, 1.0)`、`maxnumf(NaN, 1.0)`、`maximumnumf(NaN, 1.0)` 各是什么？
7. `maximumf(-0.0, +0.0)` 与 `maxnumf(-0.0, +0.0)` 的差别？
8. `arith.constant true` 的结果类型是什么？文本为什么可以省 `: i1`？
9. `mulsi_extended %a, %b : i32` 有几个结果、几个类型？
10. `cmpi` 的谓词能写成字符串 `"eq"` 吗？
11. `arith.trunci %a overflow<nsw, nuw> : i32 to i16` 合法吗？
12. `bitcast` 能改变位宽吗？
13. `index_cast` 与 `index_castui` 的区别？
14. `arith.select` 的条件类型可以是什么？
15. `arith.divsi` 能无条件 speculative 执行吗？
16. 为什么 arith 没有 `negsi`？
17. `scaling_extf` 的 `scale` 操作数是什么格式？
18. `fastmath<fast>` 与 `fastmath<reassoc>` 的区别？

**答案**

1. 能，`Arith_SignlessIntegerOrIndexLike` 包含 `index`（`ArithOps.td:27`）。
2. `-3` 与 `-4`（向零 / 向下）。
3. 负（符号随被除数），结果 `-1`。
4. poison；`overflow<nsw>` 是"不溢出"的断言，不是回绕开关。
5. 两个结果：和 + `i1` 溢出位，**不 poison**。
6. NaN / `1.0` / `1.0`（传播 / 忽略 / 忽略）。
7. `+0.0` / 任选（`maxnumf` 对 ±0 不指定）。
8. `i1`；BoolAttr 自带类型，格式串只有 `attr-dict $value`（`ArithOps.td:263`）。
9. 2 个结果、1 个类型（第二个结果类型由第一个决定）。
10. 能，enum 谓词有裸词与字符串两种拼写（`canonicalize.mlir:3120`）。
11. 合法，cast 类有 overflow 槽位（`ArithOps.td:1571` 一代）。
12. 不能，同宽重解释。
13. `index → iN` 时按有符号 / 无符号解释。
14. `i1`，或元素为 `i1` 且与结果同形状的 `vector`/`tensor`。
15. 不能，除零与 `INT_MIN / -1` 是 UB（`ConditionallySpeculatable`）。
16. `subi 0, x` 已足够；arith 只保留有独立语义/独立文本的 op。
17. E8M0 的块缩放（OCP MXFP），需广播到与输入同形状。
18. `fast` 是一组标志的集合（含 `reassoc`）；`reassoc` 只允许重结合。

## 9. 七个动手实验

| # | 实验 | 产物 |
| --- | --- | --- |
| 1 | custom ↔ generic 往返 | 10 条语句的两种文本对照 |
| 2 | `addi %c0, %x` → `%x` | 指出这是 folder 还是 pattern（`ArithOps.cpp` 的 `AddIOp::fold`） |
| 3 | `--int-range-optimizations` 前后的 diff | 每条变化对应哪条 range 规则 |
| 4 | `--arith-int-range-narrowing` | i32 → i8 的推导链 |
| 5 | `--arith-expand` 展开 `maxsi`/`maximumf` | 展开后的 op 序列 |
| 6 | `--arith-emulate-wide-int` 拆 i128 | 2×i64 的进位/乘法结构 |
| 7 | 差分测试 | `arith-check.py` 与 `mlir-opt` 判定不一致的语句清单 |

## 10. 命令与资源

```bash
# 往返与文本
mlir-opt --mlir-print-op-generic in.mlir
mlir-opt --mlir-print-debuginfo in.mlir

# 变换
mlir-opt --canonicalize in.mlir
mlir-opt --int-range-optimizations in.mlir
mlir-opt --arith-int-range-narrowing in.mlir
mlir-opt --arith-expand in.mlir
mlir-opt --arith-unsigned-when-equivalent in.mlir
mlir-opt --arith-emulate-wide-int in.mlir

# 本仓库的参考实现（26 章）
cd ana-docs/mlir && python3 tools/arith-check.py
```

| 资源 | 位置 |
| --- | --- |
| 权威语义 | `ArithOps.td` 每个 def 的 `description`（文档站由它生成：`IR/CMakeLists.txt:9` 的 `add_mlir_doc`） |
| 正面语料 | `mlir/test/Dialect/Arith/ops.mlir`、`canonicalize.mlir` |
| 负面语料 | `invalid.mlir`（145 条 `expected-error`，其中 8 条是语法层，其余是 verifier） |
| 分析接口 | `InferIntRangeInterfaceImpls.cpp`、`ValueBoundsOpInterfaceImpl.cpp` |
| 降级 | `lib/Conversion/ArithToLLVM/ArithToLLVM.cpp` |
| 文本 | 26 章的 `arith-grammar.y`（8 类语句）+ `tools/arith-check.py` |

## 11. 一句话收束

**arith 小在词汇（56 个 op、8 类语句），大在语义**：三种 NaN、四种除法方向、
两种溢出表达（poison vs 扩展结果）、一条 `index` 通道、一组 fastmath 旗标。
精通它不靠背 API，靠三件事——**能预测 fold、能解释 poison、能判断新 op 该不该进来**。
