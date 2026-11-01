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
表在当前目标是安全的；换到 AArch64 就必须先让 kind 进键。

"规则不读 immediate"不是独立条件：读 immediate 只能经 opaque 谓词，而
opaque 整组不表化。

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

## 10. 未验证 / 待办

- D-71 §5 和 12-ast-analysis §7 的原文未读，本文只核对它们概括的判据；
- "AArch64 开答案缓存会重现 a076" 是读代码得出的结论，没有做实验；
- `f64` 与 `s64` 在 extended LLT 下共用 scalar 类的现象没有单独写测试；
- AArch64 的 7 张表没有 lit 级自检。
