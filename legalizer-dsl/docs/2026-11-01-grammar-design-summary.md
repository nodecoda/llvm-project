# 2026-11-01  语法设计总结（v8）

这一轮把 `legalize-decl` 从“能写出全部 63 个 X86 规则集”推到“能说出这些规则
在什么目标下成立、以及这句话能不能被检查”。下面记的是设计本身，不是改动日志；
改动日志在 `git log -- legalizer-dsl`。

---

## 1. 这份语言是什么，不是什么

**是**：一条 legalizer 规则的**声明**。写清楚“哪些形状是终点（legal）、不在终点
往哪走（prefer）、没有专门规则时按什么兜底（policy）”，以及“以上在什么环境下成立
（env / table over）”。

**不是**：不是字节码，不是可编程的规则引擎。没有 salience，没有 agenda，没有
“后写的覆盖先写的”。**具体度决定结果，文字顺序不决定结果**。

**只诊断，不执行**。语言描述规则；跑规则的是 C++ 的 legalizer。

---

## 2. 分层

```
env { mode / feature / let }     ← 这份规则在什么目标下成立
table over …                     ← 生成的表承诺覆盖哪一类环境
macro / class                    ← 定义（类型位）
op … { query / legal / prefer / policy / except / follow / … }
expr                             ← 唯一的表达式文法
```

前四层各有各的读者：`env` 只给离线分析看，`table over` 只给生成器看，`macro`
在解析期就被替换掉，`op` 块是唯一进入测量的东西。`expr` 不是第五层，是它们
共用的表达式文法。

---

## 3. 八条设计原则（每条都附上促成它的那个错）

### 3.1 一个符号一个职责

| 符号 | 职责 |
|---|---|
| `[ ]` | 有序列表（位置有意义） |
| `{ }` | 块 / 作用域 |
| `( )` | 表达式分组；product（元组） |

`legal [s8, s16];` 是**语法错误**（`13-legal-bracket`）：legal 子句是 product 的
并，不是列表。读起来像列表就会有人当列表用，所以宁可不接受。

### 3.2 格式先行

`query ty 0, ty 1;` 声明查询形状，然后两个方向都查：提到未声明 = error
（`13-ty-undeclared`），声明未提到 = warning（`13-slot-unconstrained`）。
以前 arity 靠 `chain_arity` 读链或 `slot_count` 读文本推出来，删掉一条 slot-1
规则会让查询空间**静默缩小**。

### 3.3 事实 / 偏好 / 默认，三类分开

- `legal` 是**事实**：这些形状合法。逗号是并，元组是 product。
- `prefer` 是**偏好**：不在终点时往哪走（`s32 on 1`、`pow2 min 32`、`elem`）。
- `policy` 是**默认**：没有专门规则时按什么兜底，界可以读合法集（`hull`）、
  可以是区间（`[lo, hi]`）、可以依赖配置（`cond(HasSSE2, s64, s32)`）。

三者混写会让“这条规则为什么在这里”无法回答。`hull` 在投影为空时是**错误**而不是
静默不动——静默不动的产物就是 UNMERGE 的 144 点和 LOAD 的 50 点。

### 3.4 不写槽位 = 默认规则；特指优先；顺序无关

```
prefer s8;          -- 默认：没被特指认领的槽
prefer s32 on 1;    -- 特指：槽 1，赢过默认
```

两个声明争夺同一槽 = **错误**（`%nonassoc`），不是“后写的赢”。这条替代了
“可编程优先级”：我们把规则顺序完全反转，只有 4.9% 的点变化；改成“最具体者优先”
只变 0.1%——说明这套规则本来就大体按具体度排序，那么就把具体度写成语义。

### 3.5 `let` 是定义，要消去；不是约束

```
let HasCMOV = cmov || HasSSE1 || Is64Bit;
```

枚举之前替换掉，所以 `HasCMOV` 永远不是一个采样维度。另一条路——保留名字、加等式、
枚举时过滤——要求**每一个**枚举环境的地方都记得过滤；`realizable` 就是这么漂移的
（把 `hasCMOV() || hasSSE1() || is64Bit()` 在 64 位下恒真的值当成了自由维度，
301 个环境里有 6 个不可能存在）。

### 3.6 没有 `assume`；只有 `table over`

```
table over Is64Bit;
```

这是**承诺**：这份表覆盖哪一类环境。生成器必须据此产出 runtime 闸门。今天的
`if (X86GISelTable && STI.is64Bit())` 是同一个事实的第二份手写副本，两者会漂移。

**`assume` 被明确拒绝**：`llc -mtriple=x86_64 -mattr=-sse2` 退出码 0，所以
“x86-64 蕴含 SSE2”是默认值不是事实。把可关掉的默认值写成不变式，就是让一个开关
看起来像一条真理。

### 3.7 域的构造只有一处；模式集是参数

`envs_for()`：幂集 → 可达 → 模式 → 商，返回四个计数器。以前两个工具各建各的域
（301 被测量 vs 602 被检查），没人比较它们。

模式集**按调用点传**，没有全局默认：

| 调用点 | 模式 | 理由 |
|---|---|---|
| 点测量 | `{Is64Bit=1}` | 两边都来自己烘死的 64 位证据（`sMaxScalar`、`p0:64`） |
| 答案覆盖 | 两种 | 读 block 自己的界，32 位就用 32 位的 `sMaxScalar` |
| 5/6/9 检查 | 两种 × 可达 | 只读 block，不碰类型全域 |

漏传 = “全部模式”，所以漏一个参数不会悄悄砍掉一半域。

### 3.8 每个 0 都要能变红

这一轮里“绿着但没在测”的情况抓到过四次：自检自己跟自己比、只比身份不比内容、
计数器被同名变量遮蔽恒为 0、警告来自不可能存在的配置。所以现在的规矩是：
每个 0 都配一个能变红的控制，控制本身也要能被变异破坏。

---

## 4. 被拒绝的设计

| 提案 | 为什么拒 |
|---|---|
| 可编程优先级（salience / agenda） | 反转顺序只影响 4.9% 的点；具体度才是真的，那就把具体度写成语义 |
| `assume 条件` | 大部分“事实”其实是默认值（见 3.6） |
| `let` 作为约束（加等式 + 过滤） | 要求每个枚举者记得过滤（见 3.5） |
| 通用环境注入 / 按 env 参数化整张表 | LLT 自带 env（`p0:64` vs `p0:32` 是两个不同的查询），真正查不见的只有 `sMaxScalar` 一个 |
| `legal [a, b]` | 列表 ≠ product 的并（见 3.1） |
| 商到“链的终点” | 更粗，但要能证明终点等价；现在做不到，先商到“block 读到的表达式” |

---

## 5. 版本是叠加的

| 版本 | 加了什么 | 旧文件 |
|---|---|---|
| v6 | `op` / `legal` / `prefer` / `except` / `follow` / `macro` / `class` / `elemwise` | — |
| v7 | `query` / `variadic` / `policy` / `imm` / `group` / `with` | 逐字节同义 |
| v8 | `env` / `table over` | 不写这两样 = v7 语义 |

不写 `env { }` 的文件仍然从文本里猜原子。这不是遗留，是**对照**：
`data/2026-11-01-x86.decl`（v7）与 `data/2026-11-01-x86-env.decl`（v8）的 63 个
规则体逐字节相同，只差头部。

---

## 6. 每条语法对应的检查点

| 语法 | 检查 | 码 |
|---|---|---|
| `env` | 用了没声明的原子 | `13-env-undeclared` |
| `let` | 成环 / 体里有未声明的名字 | `13-let-cycle`、`13-let-undeclared` |
| `let` | 声明了没人用（在消去**之前**判断） | `13-env-unused`（warn） |
| `table over` | 点了没声明的原子 | `13-table-over-unknown` |
| `query` | 重复 / 跳号 / 顺序 / 提到未声明 | `13-query-*`、`13-ty-undeclared` |
| `group` / `with` | 未知组 / 嵌套非子集 / 块里放位置性语句 | `13-with-*` |
| `policy` | 覆盖冲突 / 不可达 / 死配置 / 无不动点 | `13-policy-*` |
| `class` | 未定义 / 空 / 非标量 / 用在 mem 块 | `12-class-*` |
| `prefer` | 瞬态目标 / 目标不在全域 / 无法求值 | `6-prefer-*` |

---

## 7. 测量

同一份规则，只换头部：

```
v7  data/2026-11-01-x86.decl       44385 / 46585  (95.3%)
v8  data/2026-11-01-x86-env.decl   43240 / 45433  (95.2%)
```

**这两行的百分比不能直接比**（分母变了）。可比的是域本身：

```
v7  1688 raw = 301 unmeasured + 1086 pruned + 301 measured
v8   846 raw = 135 unmeasured + 442 pruned + 269 measured
```

`unmeasured` 是“可达但类型全域表示不了”的 32 位配置——以前这个数根本不出现在
报告里。商的收益（都来自 `infer_defaults` 自己的输出）：

```
v7  301 measured -> 157 classes   (merged 144)
v8  269 measured -> 155 classes   (merged 114)
G_FPEXT   6 envs -> 2 classes     (v7；v8 是 4 -> 2)
G_ADD     6 envs -> 5 classes     (v7；v8 是 8 -> 5)
```

**G_ADD 只合掉 1 个，这是对的结果**：它的 `legal` 集真的按 HasSSE2 / HasAVX2 /
HasAVX512 / HasBWI 分四档，四个环境给出的答案不同。商只合并“block 读不出差别”
的环境；把 G_ADD 也合成 1 类，就是环境版的地址空间 bug。

---

## 8. 已知缺口（都记了数）

| 缺口 | 现在是多少 |
|---|---|
| 32 位 | 135（v8）/ 301（v7）个可达配置未测；`U` 里没有 `p0:32`、`v16p0` |
| `table over` 的闸门 | 仍是手写 C++，没有生成 |
| 商到终点 | 未做；现在商到“block 读到的表达式” |
| 跨槽规则 | LOAD / STORE / EXTRACT / INSERT 的原子划分（`rule_atoms.py`）只测了 30 个单槽规则集，31 个跨槽跳过 |
| `legal` 里的 `cond` | 不做商（会把形状的值漏进签名），语料里今天没有 |

---

## 9. 下一步（已对齐）

| 步骤 | 内容 | 负控（必须能变红） |
|---|---|---|
| S3 | `table over Is64Bit;` → 生成器产出 runtime 闸门，删掉手写的 `if (X86GISelTable && STI.is64Bit())` | 把声明改成 `table over Is64Bit, HasSSE2;`，`llc -mtriple=x86_64 -mattr=-sse2 -x86-gisel-table` 必须从“用表”变成“回退链” |
| S4 | 回到主线：格子（env 类 × 类型类）→ MIR 用例 → 与链对拍 | 每个格子至少一条用例；漏一个格子时覆盖率要打印出来 |

明确不做（不是遗漏）：

- 32 位：`U` 里没有 `p0:32`、`v16p0`，是硬阻塞，不是偷懒；
- `X86_STORE` 的真 escape：需要 `typeDiffersFromMemDesc` 谓词叶子，与 S3 同批做；
- `Answer::Escape` / `kOpaqueCell` 的死状态：今天靠 fail-closed 保证走不到。

---

## 10. 复现

```bash
tools/check_grammar.sh                                   # bison 0/0/0 + v7/v8 正例 + 3 负例
python3 tools/check_decl.py --self-test                  # 11+27+12+10+7+4+2 项
python3 tools/check_decl.py data/2026-11-01-x86-env.decl  # 0 errors, 1 warning
python3 tools/infer_defaults.py data/2026-11-01-x86-env.decl
python3 tools/infer_defaults.py data/2026-11-01-x86.decl --self-test
python3 tools/check_policy_ab.py                          # 8 条断言
python3 tools/build_decl.py | diff - data/2026-11-01-x86.decl
python3 tools/build_decl.py --env | diff - data/2026-11-01-x86-env.decl
```
