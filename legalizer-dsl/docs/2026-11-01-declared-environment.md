# 2026-11-01  声明的环境（v8）

v7 以前，构造期特征是从 `.decl` 文本里**猜**出来的：扫标识符，减去类型、宏、
类、查询谓词和一张黑名单。猜法把三件不同的事压成一层，代价是可测的。

## 1. 三层，不是一层

| | 例子 | 来源 | 在模型里 |
|---|---|---|---|
| `mode` | `Is64Bit` | triple | 改**类型全域**（`p0:64` vs `p0:32`）和宏，不只是 guard |
| `feature` | `HasSSE2`、`HasAVX512` | `-mcpu` / `-mattr`，**每个函数可变** | guard 的一个自由布尔 |
| `let` | `HasCMOV = cmov \|\| HasSSE1 \|\| Is64Bit` | 别的谓词的函数（`X86Subtarget.h:208`） | 被替换掉，**不是维度** |

压成一层的两个后果，都能用代码证伪：

* `canUseCMOV()` 是 `hasCMOV() || hasSSE1() || is64Bit()`，所以
  `{HasCMOV=0, Is64Bit=1}` **不可能**。v7 把它当可达环境枚举：301 个 64 位
  环境里有 6 个（G_SELECT 的一半）。
* `feature_atoms_of` 把**整个文件的宏体**拼进**每个** block，所以 63 个 block
  里有 37 个继承了 `{Is64Bit, HasAVX, HasAVX512}`——它们自己一个都没提。

## 2. 文法

```
env {
  mode    Is64Bit from triple;
  feature HasSSE1, HasSSE2, HasAVX512, cmov, bwi, x87, softfloat from mattr;

  let HasCMOV = cmov || HasSSE1 || Is64Bit;
  let UseX87  = !softfloat && x87;
  let HasBWI  = HasAVX512 && bwi;
}

table over Is64Bit;
```

* `let` 是**定义**，语义是**消去**：枚举之前替换掉，所以定义的名字永远不会
  成为一个采样维度。写成「保留名字 + 加等式 + 枚举时过滤」是另一种设计，
  它要求每个枚举环境的地方都记得过滤——`realizable` 就是这么错的。
* `table over` 是**承诺**，不是开关：生成器必须由它生成 runtime 闸门。
  今天 C++ 里手写的 `if (X86GISelTable && STI.is64Bit())` 是同一个事实的
  第二个副本，两者会漂移。
* **没有 `assume`。** `llc -mtriple=x86_64 -mattr=-sse2` 退出码 0，所以
  「x86-64 蕴含 SSE2」是默认值不是事实；写成假设就是把一个能关掉的东西
  钉成不变式。哪些配置被覆盖是 `table over` 的事。

## 3. 检查（都在 `check_decl`）

| 码 | 含义 |
|---|---|
| `13-env-undeclared` | block 用了没声明的原子（v7 里它是静默的额外维度） |
| `13-let-cycle` | 定义成环（否则替换不终止） |
| `13-let-undeclared` | `let` 体里出现未声明的名字 |
| `13-table-over-unknown` | `table over` 点了没声明的原子 |
| `13-env-unused`（warn） | 声明了但没人用；**在消去之前**判断 |

`env_self_test()` 三条，其中两条有变异负控：关掉 `let` 消去之后必须报
`FAIL env: ['HasBWI', 'HasCMOV', 'HasDQI', 'UseX87'] sampled but not a declared
primitive` 和 `4 environments have HasCMOV=0 at Is64Bit=1`。

## 4. 环境商：同义的环境只问一次

枚举原子的幂集会把**同一个问题问很多遍**：G_FPEXT 在 64 位下有 6 个可达环境
（v7），答案只有 2 种。所以采样域不是幂集，是**商**：

```
env₁ ~ env₂  ⟺  Read(b, env₁) = Read(b, env₂)
Read(b, env) = 该 block 从环境读到的全部取值
             = 每条 legal/except 守卫的真值
             + 每条 prefer 守卫的真值 **和目标的值**（cond 会产出不同的类型）
             + 每条 policy 的守卫、界
```

两条纪律：

* **签名必须是值的向量，不只是真值。** 目标里的 `cond(HasSSE2, s64, s32)`
  产出的是类型，不是真假；只取真值会把两个答案不同的环境合并——这正是
  地址空间那个 bug 的形状（`classifyType` 只看 kind 和位宽）。
  自检里有一条构造出来的反例：一个 agent 没被任何守卫提到的 `cond` 目标，
  **粗签名合并成 1 类，细签名分成 2 类**，而且粗类里确实装着两个不同的答案。
* **`legal` 形状里出现 `cond(` 的 block 不做商**（今天没有）。那需要把形状的
  值也放进签名，而形状可能是 product；宁可退回幂集，不猜。

声明的 **soundness 由不是签名的东西检查**：G_FPEXT 上逐个环境跑链的终点，
必须等于它代表元的终点——用签名去验签名就是自检自己跟自己比。

## 5. 测量（同一份规则，只换头部）

```
v7  data/2026-11-01-x86.decl       44385 / 46585  (95.3%)   dropped 543  merged 144
v8  data/2026-11-01-x86-env.decl   43240 / 45433  (95.2%)   dropped 272  merged 114
```

**上一版这里写的「93.5% → 96.5%」是分母变动的假象**，不是改进：v8 的字母表
变小了，分母跟着变小，比例不能直接比。引入环境商之后两个数才可比——结论
也变了：

* 百分比基本持平（95.3% vs 95.2%），
* **v8 的收益是分母更小**：45433 个问题 vs 46585（少 2.5%），而且那 844 个
  被 `Is64Bit` 过滤掉的环境现在**出现在报告里**：

```
arithmetic: 1688 raw = 844 outside the measured mode + 543 pruned by implies + 301 measured
```

四个计数器必须划分幂集，加不起来自检就失败。这是报告以前做不到的事——
三个数印在两个地方，没有任何东西比较它们。

63 个 block 的规则体逐字节相同（`build_decl --env` 只加头部），所以差异
全部来自环境模型。按 block 看：

| 变化原因 | block | 分母 |
|---|---|---|
| 去掉没人读的宏维度（3 个） | `G_ANYEXT`、`G_CTPOP`、`G_MERGE_VALUES`、`G_TRUNC`… | 1728 → 576 |
| `UseX87` 拆成 `softfloat × x87`，再被商合并 | `G_FPEXT`、`G_FCMP`、`G_SELECT`… | 4608 → 9216 → 商回 1536 |
| 字母表没变（对照） | `G_EXTRACT`、`G_INSERT`、`G_LOAD`、`G_STORE` | 不变，逐点相同 |

## 5.1 一份域，两个模式集

`envs_for()` 是唯一的域构造函数（幂集 → 可达 → 模式 → 商），返回四个计数器。
两个工具不再各建各的：

| 调用点 | 模式集 | 为什么 |
|---|---|---|
| `infer_defaults` | `MODE['measured']` = `{Is64Bit=1}` | 两边都来自 `x86-core.json`，它的合法集是 64 位 subtarget 的答案（`sMaxScalar` 已烘死、指针是 `p0:64`） |
| `policy_coverage` | `MODE['all']` | 它读 block 自己的界，32 位配置就用 32 位的 `sMaxScalar`，与 `U` 无关 |

模式集是**参数**，不是全局默认：漏传就是“全部模式”，不会悄悄少一半。

**顺带清掉 4 条幽灵警告。** `5/6/9` 那组检查以前枚举**全部**幂集，包括
`HasSSE1=0,HasSSE2=1` 这种被 implies 表判死的配置，于是
`6-prefer-transient` 在不可能的配置上报警。语料警告数 5 → 1（剩下的那条是
`7-diagonal-needs-follow`）。这组检查不读 `U`，所以它用**全部模式 × 可达**，
与点测量不同——这是三个不同的域，现在每个都在调用点写清楚了。

## 5.2 没有做的

* 没有 32 位：`U` 里没有 `p0:32` / `v16p0`，测不了，不是「被丢弃」。
* 没有生成 `table over` 的闸门：C++ 侧仍是手写。
* 商的粒度是「block 读到的表达式」，不是「链的终点」。后者更粗（会把
  `Read` 不同但答案相同的环境也合并），但需要能证明终点等价，现在没有。

## 6. 复现

```bash
tools/check_grammar.sh                                  # bison 0/0/0 + v7/v8 + 3 负例
python3 tools/check_decl.py --self-test                 # 11+27+12+10+7+3
python3 tools/check_decl.py data/2026-11-01-x86-env.decl # 0 errors, 5 warnings
python3 tools/infer_defaults.py data/2026-11-01-x86-env.decl
python3 tools/infer_defaults.py data/2026-11-01-x86.decl --self-test   # 含商/分区负控
python3 tools/build_decl.py | diff - data/2026-11-01-x86.decl          # 逐字节
python3 tools/build_decl.py --env | diff - data/2026-11-01-x86-env.decl
```
