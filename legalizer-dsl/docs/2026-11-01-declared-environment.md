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

## 4. 测量（同一份规则，只换头部）

```
v7  data/2026-11-01-x86.decl       79857 / 85419   (93.5%)   domain dropped 543
v8  data/2026-11-01-x86-env.decl   74403 / 77089   (96.5%)   domain dropped 272
```

63 个 block 的规则体**逐字节相同**（`build_decl --env` 只加头部），所以差异
全部来自环境模型。分母变了，所以两个百分比**不能直接比**；可比的是按 block：

| 变化原因 | block | 分母 |
|---|---|---|
| 去掉没人读的宏维度（3 个） | `G_ANYEXT`、`G_CTPOP`、`G_MERGE_VALUES`、`G_TRUNC`… | 1728 → 576（3×） |
| `UseX87` 拆成 `softfloat × x87`（更精确，也更大） | `G_FCMP`、`G_SELECT`、`G_FPEXT`、`G_FPTRUNC`、`G_SITOFP`、`G_FPTOSI` | 4608 → 9216（2×） |
| 字母表没变（对照） | `G_EXTRACT`、`G_INSERT`、`G_LOAD`、`G_STORE` | 不变，逐点相同 |

**这两股力量方向相反**，净值 −10%。第二股是下一步要收的：`softfloat`/`x87`
只在 `UseX87` 里出现，它们 4 种组合里只有 2 种行为不同，所以应该按「block
读到的所有表达式的联合取值」做商，而不是按原子的幂集枚举。实测这个商是
`135 → 49`（28 个单槽 block）。

## 5. 没有做的

* 没有做环境商（上面那条）。
* 没有 32 位：`U` 里没有 `p0:32` / `v16p0`，测不了，不是「被丢弃」。
* 没有生成 `table over` 的闸门：C++ 侧仍是手写。
* `infer_defaults` 的 `domain: N dropped` 只统计 implies 剪枝，**不含**被
  `Is64Bit` 过滤掉的那一半（v7 上 844 个，报告里一个数字都没有）。

## 6. 复现

```bash
tools/check_grammar.sh                                  # bison 0/0/0 + v7/v8 + 3 负例
python3 tools/check_decl.py --self-test                 # 11+27+12+10+7+3
python3 tools/check_decl.py data/2026-11-01-x86-env.decl # 0 errors, 5 warnings
python3 tools/infer_defaults.py data/2026-11-01-x86-env.decl
python3 tools/build_decl.py | diff - data/2026-11-01-x86.decl          # 逐字节
python3 tools/build_decl.py --env | diff - data/2026-11-01-x86-env.decl
```
