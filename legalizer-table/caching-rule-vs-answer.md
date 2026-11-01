# 缓存规则号，而不是答案

版本：`sync/table-classify` @ `b2b2b62815ba`，2026-11-01。

## 0. 边界

本文的结论只用本树的代码和提交核对过。文中引用的
`D-20261101-71-aarch64-port-and-cached-rule.md` §5 和
`12-ast-analysis.md` §7 属于别的树（`llvm-dev` / `dsl-tablegen`），
**没有读取，也没有核对原文**；本文只分析它们所概括的那条判据本身。

## 1. 问题

合法性表的格子是查询空间的一个商：同一个格子里的查询被当成"同一个问题"。
格子该存什么？

- 存 `LegalizeActionStep`（答案）= 把 fill 时那个代表类型得到的答案强加给整格；
- 存 `int64_t`（规则号）= 只记住"该问哪条规则"，答案由真实查询重放得到。

`a076fbbf9e72` 把前者换成后者，理由是"mutation 是查询的函数"。

## 2. 事实核对（本树）

`a076fbbf9e72` 的 diff（4 个文件，没有测试改动）与上面的描述一致：

```
-  LegalizeActionStep Step = ...apply(representativeQuery);
-  Steps.push_back(Step);
+  const int64_t RuleIdx = ...findRule(representativeQuery);
+  RulesOfCell.push_back(RuleIdx);

   // query
-  const LegalizeActionStep &S = Steps[Cell - 1];
+  if (RuleIdx >= 0 && !RS.ruleMatches(RuleIdx, Q)) { ++Splits; return Split; }
+  const LegalizeActionStep S = RS.applyRule(RuleIdx, Q);
```

当前 HEAD `b2b2b62815ba` 不是"纯规则号"形态，也不是"纯答案"形态：

```
RulesOfCell[i] = findRule(representative)   // 63 张表
Answers[i]     = applyRule(...)             // 仅 OneQuestion 的表，56 张
```

7 张表（LROUND、LOAD、STORE、SEXTLOAD、ZEXTLOAD、FPEXTLOAD、FPTRUNCSTORE）
仍走规则 replay。`table-check.ll` 里的 `add128` 不是 a076 加的，是
`f3acadafe20d` 加的（`git log -S add128`）。

注意：另一条分支（`feat/legalizer-answers`）确实是"生成 `X86Answers[]`、
没有重放机制"的形态；不要把那条分支的现状当成这条分支的现状。

## 3. 原理：商、规则号、答案

表把查询映射到格子，等于定义了一个等价关系 `≈`。缓存合法的唯一条件是：

> **被缓存的东西必须对 `≈` 的每个等价类恒定。**

对谓词成立：

- 生成器收集每个谓词叶子读的槽，键里全部包含；
- 不认识的叶子 `PrintFatalError`；opaque 谓词整组不表化；
- 规则读整个 `MemoryTy` 时，mem 轴改按类型类切，而不是按 size；
- 子目标相关的 `hasFeature` 不进键，因为 `X86LegalizerInfo` 每个 subtarget
  一个实例，`Subtarget.hasAVX()` 在构造时已经是实例里的常量。

所以 `findRule(Q)` 在一个格子内恒定，缓存规则号合法。

对答案不成立：mutation 是 `Q` 的函数，而且它会保留一些被商丢掉的位。

```
changeElementSize(NewSize):
  if isInteger()  -> LLT::integer(NewSize)
  if isFloatIEEE()-> LLT::floatIEEE(NewSize)
  else            -> LLT::scalar(NewSize)
```

商把 `INTEGER` / `FLOAT` / `ANY_SCALAR` 都归成 `Kind_Scalar`
（`classifyType` 用 `T.isScalar()`），所以格子的代表是 `LLT::scalar(128)`
（打印 `s128`），而真实查询可能是 `LLT::integer(128)`（打印 `i128`）。
两者比较相等，但 mutation 的输出拼写不同：

| 查询 | 答案 |
|---|---|
| `s128`（代表元） | `s64` |
| `i128`（真实查询） | `i64` |

缓存答案 = 把 `s64` 发给 `i128`。缓存规则号 = 每个查询自己重放，各得各的。

一句话：**缓存规则号不是风格，是"商不能丢答案函数看得见的东西"这条规则的
直接推论；当商确实粗了，只能把被丢掉的输入留给运行时重算。**

## 4. 为什么对拍看不见：`LLT::operator==` 的通配

```cpp
// LowLevelType.h
bool operator==(const LLT &RHS) const {
  if (Info == RHS.Info && RawData == RHS.RawData) return true;
  if (isAnyScalar() || RHS.isAnyScalar())
    return isScalar() == RHS.isScalar() &&
           getScalarSizeInBits() == RHS.getScalarSizeInBits();
  ...
}
```

`LegalizeActionStep::operator==` 用的就是它，所以 `i64 == s64` 为真。
逐查询比较 `(Action, TypeIdx, NewType)` 永远看不见这个 bug，只有生成的 MIR
文本不同。

而且这个区别只在**启用 extended LLT** 的目标上存在：AArch64、AMDGPU、
WebAssembly 调 `LLT::setUseExtended(true)`；X86 没有。本树实测 `add i128`：

```
x86_64  -global-isel -stop-after=legalizer  →  %13:_(s64), %19:_(s8) = G_UADDO ...
aarch64 -global-isel -stop-after=legalizer  →  %13:_(i64), %19:_(i32) = G_UADDO ...
```

所以 a076 描述的是 AArch64 的现场（与 D-71 的文件名一致），在 X86 上复现不了；
这也解释了它为什么没有随提交带测试。

## 5. 守卫：`ruleMatches` 不够

`ruleMatches(RuleIdx, Q)` 只证明"缓存的规则还匹配"，**不证明没有更早的规则
也匹配**。而链答的是 first-match。

- 运行时把 `Split` 当 canary：规则不再匹配就拒绝回答、回退链。它能抓
  "键把两个问题合并了"的一种表现；
- 但如果另一条更早的规则也匹配 `Q`，`ruleMatches` 仍为真，表会答另一条规则的
  答案——`Split` 抓不到，答案对拍也可能因为答案恰好相同而抓不到；
- 唯一充分的测试是 `findRule(Q) == RuleIdx`。

本树的取舍（`b2b2b62815ba`）：

- release 路径保留 `ruleMatches`（它的等价性由第 3 节的不变量保证）；
- `-x86-gisel-table-check` 下，replay 路径额外比较 `findRule(Q)` 与缓存索引，
  把不变量本身在真实查询上测掉；
- 严格方案实测过：release 改 `findRule` 后 legalizer 收益从 −3.6% 降到
  −3.05%，lookup 从 2.30× 降到 1.47×。

## 6. OneQuestion 的准确条件

缓存答案只在"格子可证明只有一个问题"时成立。本树的实际条件：

| 条件 | 说明 |
|---|---|
| 无 mem 轴 | mem 轴是一个 fiber，不是一个点 |
| `Slots[K] == K` | 键覆盖全部类型槽，且按顺序 |
| 写槽 ⊆ 键槽 | 否则答案的 TypeIdx 取决于 fill 时那个查询 |
| **读槽 ⊆ 键槽** | `sameAsSlotN`；`b2b2b62815ba` 补的 |
| 目标没有 extended LLT | 否则 `i128`/`s128`、`f64`/`s64` 被商掉（本文第 3 节） |

最后一条不是生成器的现有判据。X86 没有 extended LLT，所以 56 张 OneQuestion
表在当前目标是安全的；换到 AArch64 就必须先让 kind 进键，或者在运行时把答案
缓存关掉。

"规则不读 immediate"不是独立条件：读 immediate 只能经 opaque 谓词，而
opaque 整组不表化。

落地状态（`b2b2b62815ba` + 本轮）：

- 前四条由生成器/运行时强制；
- 第五条原来**没有**运行时门控（`queryImpl` 只判断 `D.OneQuestion &&
  AllowCachedAnswer`）。两条路：
  1. 运行时门控：`&& !LLT::getUseExtended()`；
  2. 让 kind 进键：classify 区分 ANY_SCALAR / INTEGER / FLOAT，类表同时
     生成三种 kind 的类。
  本轮选了路 1，理由是 X86 上 flag 恒为 false，门控只多一次 relaxed load；
  路 2 更彻底，但它改变类表的形状，属于 AArch64 线开工时要付的成本。路 1
  不排除路 2：等 kind 进键之后，门控可以删掉，答案缓存对 AArch64 重新可用。

本轮真正落地的三件事（`X86LegalizerTable.cpp` / `X86TargetMachine.cpp`）：

| 改动 | 目的 | 可红的证据 |
|---|---|---|
| `queryImpl` 的答案缓存加 `!LLT::getUseExtended()` | extended LLT 目标上自动退回重放，不再把代表元的拼写发给别的 kind | `table-extended-llt.ll` 的 `cached 0` |
| 检查模式的比较改成**按拼写**（`sameSpelling` / `sameAnswer`） | `LLT::operator==` 对 ANY_SCALAR 通配，看不见 `s64` vs `i64`；`print()` 看得见 | 同测试的 `i64` / `f64` MIR；去掉门控即 fatal（见下） |
| `-x86-gisel-extended-llt`（hidden，默认关） | 让"目标有 extended LLT"这个前提在 X86 上可复现，否则门控是死代码 | 打开后 `cached 0`、MIR 里 `i32`/`i64`/`f64` |

数字（本树实测，`-stop-after=legalizer`）：

- 打开 hook：自检 `points 2866 compared 2866 mismatches 0 skipped 0`，
  真实查询 `hit 14 … cached 0 checked 14`；i128 的 `G_UADDO` 产出
  `%13:_(i64), %19:_(i8)` —— 重放保住了 kind。
- 只删门控（保留新比较器）：立刻
  `table 1/0/s64, chain 1/0/i64 for Opcode=55, Tys={i128}`；
  这正是 a076 的现场，在 X86 上复现出来了。
- 删门控**且**把比较器换回 `operator==`：检查全绿（`mismatches 0`），
  而 MIR 是 `%13:_(s64), %19:_(s8)`。旧检查确实瞎。
- 门控的代价：`-x86-gisel-table-bench=200` 三跑，有门控
  `mixed_ns 43.95–44.42`，无门控 `43.80–44.33`，区间重叠；relaxed load
  在测量分辨率以下。默认路径的 `cached` 计数（语料 8256 函数）：
  `hit 92352 … cached 86880 checked 92352`，剩下 5472 是 7 张重放表的。

## 7. AArch64 的现状与风险

- 生成的 7 张 AArch64 表 descriptor 全部 `OneQuestion=true`；
- 但 `AArch64LegalizerTable.cpp` 没有 `Answers` / `AllowCachedAnswer`，只
  replay 规则号 → 现在安全；
- 谁把 X86 的答案缓存复制过去，就会在 `i128`/`s128`、`f64`/`s64` 上重现
  a076；
- AArch64 的 query 副本还带着 X86 修之前的 `Dims[1] > 1` 轴判断和
  `PointerBits` 参数；`-aarch64-gisel-table-verify` 没有 lit 测试
  （X86 有 `table-check.ll` / `table-addrspace.ll` / `table-check-32bit.ll`）。

## 8. 可迁移的判据

```
格子存的是答案还是规则号？
  规则号 → 键是否覆盖了所有谓词读槽？（生成器 fatal + opaque 排除 + mem 轴）
           重放守卫是否证明了 first-match（findRule），而不只是 still-matches？
  答案   → 这个格子是否可证明只包含一个问题？
           目标的 LLT 语义下，这个商是否在"答案拼写/kind"上也是单点？
自检是否用真实查询，而不只是代表元？
是否比较了生成的 MIR，而不只是 LLT/step？（LLT::== 会通配 ANY_SCALAR）
```

## 9. 复现

```bash
cd /home/ubuntu/llvm/trees/table-classify
git show a076fbbf9e72
git log -S add128 -- llvm/test/CodeGen/X86/GlobalISel/table-check.ll

cd /home/ubuntu/llvm/builds/table-classify
cat > /tmp/i128.ll <<'IR'
define i128 @f(i128 %a, i128 %b) {
  %c = add i128 %a, %b
  ret i128 %c
}
IR
./bin/llc -mtriple=x86_64-unknown-linux-gnu -global-isel -O0 \
    -stop-after=legalizer /tmp/i128.ll -o - | grep G_UADDO     # s64
./bin/llc -mtriple=aarch64-unknown-linux-gnu -global-isel -O0 \
    -stop-after=legalizer /tmp/i128.ll -o - | grep G_UADDO     # i64
```

在 X86 上复现 extended LLT 现场（本轮加的 hook，默认关闭）：

```bash
cd /home/ubuntu/llvm/builds/table-classify
T=/home/ubuntu/llvm/trees/table-classify/llvm/test/CodeGen/X86/GlobalISel/table-extended-llt.ll

# 门控在位：答案缓存被关掉，规则号重放，kind 保住。
./bin/llc -mtriple=x86_64 -global-isel -O0 -stop-after=legalizer \
    -x86-gisel-extended-llt -x86-gisel-table-check \
    -x86-gisel-table-stats $T -o /dev/null 2>&1 | grep -E 'cached|mismatch'
#   x86-gisel-table-check: points 2866 compared 2866 mismatches 0 skipped 0 ...
#   x86-gisel-table: hit 14 ... cached 0 checked 14 ...

./bin/llc -mtriple=x86_64 -global-isel -O0 -stop-after=legalizer \
    -x86-gisel-extended-llt $T -o - | grep -E 'G_UADDO|G_FADD'
#   %13:_(i64), %19:_(i8) = G_UADDO ...   (kind 保住了)

# 负控 1：删掉 queryImpl() 里的 !LLT::getUseExtended()，重编译，同一条命令
#   LLVM ERROR: ... table 1/0/s64, chain 1/0/i64 for Opcode=55, Tys={i128, }

# 负控 2：再把 sameSpelling() 换回 LLT::operator==，检查会全绿，
# 而 MIR 是 s64/s8 —— 旧比较器看不见拼写。
```

## 10. 未验证 / 待办

- D-71 §5 和 12-ast-analysis §7 的原文未读，本文只核对它们概括的判据；
- "AArch64 开答案缓存会重现 a076" 仍然是从 X86 的实验 + 共用代码推出来的：
  X86 打开 extended LLT 后确实复现（见第 6 节），但 AArch64 本身没跑过；
- `f64`/`s64` 现在由 `table-extended-llt.ll` 的 `f64`/`i64` MIR 覆盖；
- AArch64 的 7 张表仍然没有 lit 级自检；
- 路 2（kind 进键）没做，AArch64 开工前要决定是否用它替掉门控。

## 11. 人话版（先读这一节也可以）

### 一句话

**格子里该写"去几号窗口办"，不该写"第一个人办出来的结果"。**

表格把"长得像的查询"归成一堆（一个格子）。同一堆里，**该走哪条规则**是
一样的；但**规则算出来的具体结果**可能每个人都不一样。存结果 = 把第一个人
的结果复印给全堆；存规则号 = 每个人自己去窗口按自己的材料重办一遍。

### 例子 1：结果为什么会不一样

AArch64 上 `i128` 和 `s128` 在 LLVM 眼里**相等**（比较时"任意标量"只比
大小），但打印出来不同。格子里的代表是 `s128`，规则是"缩到 64 位"：

- 代表 `s128` → 链给 `s64`；
- 真实查询 `i128` → 链给 `i64`。

存答案，`i128` 拿到 `s64`——**类型相等，拼写错了**。存规则号，`i128` 自己
重放，拿到 `i64`。

这个例子只在**启用 extended LLT** 的目标上成立（AArch64/AMDGPU/Wasm）；
X86 上 `i128` 和 `s128` 是同一个值，复现不出来（实测：`add i128` 在 x86_64
的 MIR 是 `s64`，在 aarch64 是 `i64`）。

### 例子 2：逐条对拍为什么会瞎

检查器通常写"链答什么、表答什么，两个 `LegalizeActionStep` 比一下"。但
`LLT::operator==` 对"任意标量"通配，所以 `i64 == s64` 是 **true**。两边
"相等"，全绿；只有把 MIR 打出来才看得见差别。

**检查要检查到最终产物这一层，不能只比中间对象的相等性。**

### 例子 3：哪些格子必须重放

`load` 的格子按"访问大小"分：`s64`、`v2s32`、`p0:64` 大小都是 64 位，落在
同一格。规则对它们的处理不一样（要不要拆成多个小 load），所以这类格子只能
存规则号、查询时重放。只有"一个格子确实只装一个问题"的表才允许存答案——
X86 现在 63 张里 56 张属于后者。

### 例子 4：`splits` 能抓什么、抓不到什么

假设键出了 bug，把 `i32` 和 `s64` 塞进同一格，缓存了"`i32` 的规则 0"：

- 查询 `s64`：规则 0 不匹配 → 表拒绝回答、回退链，`splits++`。**能抓。**
- 但若规则 0 是"大小 32"、规则 1 是"任意标量"：代表 `s64` 时链选 1，
  查询 `i32` 时链选 0。表缓存 1，而 `ruleMatches(1, i32)` 仍为 **true**
  （规则 1 确实匹配 i32），表就拿规则 1 答了 `i32`。**抓不到**——规则还
  匹配，只是它不是第一条。

所以 `splits` 是 canary，不是证明；充分条件是 `findRule(Q) == RuleIdx`。
release 保留便宜守卫、check 模式做严格比较，是实测后的取舍（严格版把
legalizer 收益从 −3.6% 拉到 −3.05%、lookup 从 2.30× 拉到 1.47×）。

### 例子 5：单点格子为什么可以存答案

格子如果确实只包含一个问题（同一 opcode、所有类型槽被键精确固定、无 mem
轴、规则读写不越界），"第一个人的结果"就是"所有人的结果"，存答案没有歧义，
还省掉重放。这就是 `f3acadafe20d` 加回来的窄条件。

但要补一句它没写的：**在 extended LLT 的目标上，"一格只装一个问题"还不够**
——`i128`/`s128`、`f64`/`s64` 仍会被归进同一格。所以可迁移的判据是：

> 这个格子在"目标实际使用的 LLT 语义"下，是不是单点？

X86 是；AArch64 不是。AArch64 现在没开答案缓存，所以安全；谁把 X86 这段
复制过去，就会原样重现这个 bug。

### 记住三句

1. 格子是"问题的堆"；堆里所有人共用一条规则，但不共用一份结果。
2. 缓存规则号的前提是"键把决定规则的所有输入都装进去了"（谓词读槽、写的槽、
   读的槽、mem 轴）。
3. 只在"堆里确实只有一个问题"时才把规则折叠成答案；跨目标时还要问一句
   "这个堆在目标的 LLT 语义下是不是真的只有一个点"。
