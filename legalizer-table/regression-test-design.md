# 表化 legalizer 的回归测试集设计

版本：`sync/table-classify`，2026-11-02。范围：X86/AArch64 的
`LegalizerInfo` 规则链、`-gen-legalizer-table` 生成物、运行时
`X86LegalizerTable` / `AArch64LegalizerTable`，以及 `legalizer-dsl`
的本地检查工具。本文只使用本 worktree 的代码和测试作为证据。

## 0. 一句话

测试集不要把“表”当成一个孤立的查表函数来测。它有两个实现（链、
表）和一个生成器（`.td` → 链 + 表 + Core IR），回归测试必须同时证明：

- 表答的每个问题都与链一致；
- 表不答的问题回退链后结果不变；
- 键（等价商）足够细，cell 内 `findRule` 恒定；
- 生成器和消费者不漂移；
- 表仍然有覆盖率和性能。

## 1. 形式化 oracle

记查询空间为 `Q`：

- `C(q)`：链的 first-match 答案。
- `κ(q)`：表的键，通常是 `(opcode, 类型类..., mem 轴)`。
- `r(cell)`：cell 里保存的规则号。
- `T(q)`：表的结果，可能是 `Hit(answer)` 或 `Refuse`。
- `F(q)`：`LegalizerInfo::getAction(q)`，即 `Hit` 时用表，否则回退链。

### P1 Soundness

```text
T(q) = Hit(a)  =>  a ≡ C(q)
```

`≡` 比较 `(Action, TypeIdx, NewType)`，且 `NewType` 按拼写比较。
不能用 `LegalizeActionStep::operator==`：`LLT::operator==` 对
`ANY_SCALAR` 通配，`i64 == s64 == f64` 可能为真。这个比较器看不见
“缓存答案”最典型的 bug（`i128` 拿到 `s64` 而不是 `i64`）。

### P2 Fallback transparency

```text
T(q) = Refuse  =>  F(q) = C(q)
```

表可以拒绝，但拒绝不能改变最终 legalizer 行为。

### P3 Key adequacy

同一个 cell 内任意 `q, q'`：

```text
κ(q) = κ(q')  =>  findRule(q) = findRule(q')
```

如果不成立，表必须至少拒绝其中一个。`ruleMatches` 只是 canary，
不是证明；充分条件是 `findRule` 在 cell 内恒定。

### P4 Coverage

表必须在真实编译语料上答足够多的查询，并且每张表至少命中一次。
否则“全部回退链”也能通过 P1/P2，但功能已经退化成空壳。

### P5 Subtarget isolation

同一个查询在不同 subtarget 下可以有不同的 first-match 规则。表是
per-subtarget 实例的，不能跨 subtarget 复用 cell 答案。

### P6 Generator consistency

链和表必须由同一份 `.td`、同一段 `expandRule()` 生成；生成物必须可重复；
“不可表化”清单必须精确。

## 2. 测试分层

| 层 | 形式 | 主要防的 bug | 速度 |
|---|---|---|---|
| L0 生成器契约 | TableGen lit / 负例 `.td` | 未知谓词、不可表化、描述符错误、清单漂移 | 极快 |
| L1 查询空间差分 | C++ unit / `-check` | 表 vs 链语义不一致、拼写错误 | 快 |
| L2 端到端 MIR 差分 | lit，表 on/off | 回退不透明、真实 IR 上的偏差 | 中 |
| L3 对抗/变异 | 负控 flag / 合成 `.td` / 变异源码 | “测试绿着但没在测” | 中 |
| L4 语料/模糊 | `gen_ir.py` + 随机查询 | 域外查询、未采样 cell、长尾类型 | 慢，nightly |
| L5 性能/覆盖率 | benchmark | 退化、键爆炸、表失效 | 慢，独立 job |

## 3. L0：生成器契约

这一层不跑 legalizer，只跑 `llvm-tblgen`。

必须有的负例：

- 未知 predicate leaf → fatal，不能猜；
- raw C++ predicate → 不表化；
- opaque rule / 非字面量 / 类型列表选择 → 不表化；
- `typeDiffersFromMemDesc` 但没有规则写出 memory type → 不可表化；
- 3 个类型槽 → fatal（当前上限 2）；
- 重复 opcode、空 ruleset、builder 参数形状错误、mutation 方向错误；
- `legalFor` 的 product/list 形状错误；
- 别名 opcode 的映射。

必须有的正例/结构检查：

- 每个 leaf 至少一个测试；
- 每个 refusal 清单的精确内容；
- `Local` 映射：今天恒等，将来压缩时必须单射；
- `Dims`、`Slots`、`Mem` 的形状不变量：mem 轴必须最后，`Dims[K] > 0`；
- 生成物可重复：同一份 `.td` 跑两次 `-gen-legalizer-table`，逐字节一致；
- 链和表覆盖的 opcode 集合可比较，不能一边有、一边静默漏掉。

注意：链本身也是从 `.td` 生成的。如果 `.td` 写错了，链和表会“完美一致”，
但编译器行为已经错了。必须保留独立的链基准（旧手写 C++ 的黄金 MIR，或冻结的
IR → `-stop-after=legalizer` MIR）。

## 4. L1：查询空间差分

### 4.1 域内差分

对每张表：

- 枚举 `X86TableDomains` 给出的每个域点；
- 比较 `LI.getActionDefinitions(Op).apply(Q)`、`LI.getAction(Q)` 和
  `table.query(Q)` 的 `Answer` 种类；
- 断言 `Hit` 时答案按拼写完全一致；
- `Split` 只允许发生在非代表元上；代表元 `Split` 是 bug；
- `Outside/Declined/Untabulated` 时链答案被正确使用。

### 4.2 纤维差分

对每个类型类，枚举它的 preimage：

- `sN`、`iN`、`fN`、各种 FP semantics；
- `vL sN`、`vL iN`、`vL fN`；
- `p0:64`；
- `vL p0:64`。

断言：

- 每个 fiber member 都能分类回它来自的类；
- 表对 `i128` 和 `s128` 给出各自拼写的答案；
- extended LLT 下 `fiber_max > 1`；
- `fiber_skipped` 的原因可解释（例如 `G_FCMP` 会把元素压到 s8）。

### 4.3 拒绝门差分

每个 `Answer` 至少一个 witness：

- `Outside`：未命名位宽（`s33`、`s256`、`i65537`）、未命名 lane 数
  （`v3s8`）、scalable vector；
- `PointerVector`：没有对应类的指针向量；
- `Outside` 地址空间：`p1` / `p272`；
- `Declined`：memdesc 数量不匹配；
- `Outside`：mem 轴未命名的 access size / memory class；
- `Untabulated`：没有表的 opcode；
- `Split`：同一个 cell 内规则不匹配的真实查询。

### 4.4 真实查询差分

`-x86-gisel-table-check` 在编译真实 IR 时逐查询比较。测试要断言：

- `checked > 0`；
- `mismatches 0`；
- `fills > 0`；
- `hit > 0`；
- stats 计数之间的关系合理。

## 5. L2：端到端 MIR 差分

### 5.1 表 on/off 输出逐字节一致

```bash
llc -global-isel -O0 -stop-after=legalizer -x86-gisel-table=true  -o %t.on
llc -global-isel -O0 -stop-after=legalizer -x86-gisel-table=false -o %t.off
diff %t.on %t.off
```

这是最强的回归测试，同时覆盖命中、拒绝回退、未安装路径和真实 IR 组合。

### 5.2 `-x86-gisel-table-once` 负控

`once` 只答一个查询，然后全部回退链。它必须：

- 输出与 `-x86-gisel-table=false` 一致；
- `hits == 1`；
- 性能接近 chain。

### 5.3 golden MIR

on/off diff 在“两边一起错”时看不出问题，所以还要一组固定的期望 MIR：

- `Legal`：G_ADD i32；
- 标量 widen/narrow：G_ADD i128、G_MUL i64；
- 向量：G_ADD `<4 x i32>`、G_MUL `<8 x i16>`；
- 内存：G_LOAD/G_STORE 标量、向量、扩展 load、截断 store、volatile、
  非对齐、原子；
- 指针：G_PTR_ADD、G_PTRTOINT、G_INTTOPTR、非零地址空间；
- 转换：G_FPEXT、G_FPTRUNC、G_FPTOSI、G_SITOFP、G_FCMP；
- 位运算：G_CTPOP、G_CTLZ、G_CTTZ、G_BSWAP；
- 多类型槽：G_UADDO、G_SELECT、G_EXTRACT、G_INSERT；
- variadic：G_MERGE_VALUES、G_UNMERGE_VALUES、G_BUILD_VECTOR、
  G_CONCAT_VECTORS；
- 无类型：G_BR；
- libcall/lower/custom：G_FCOS、G_LROUND、G_UMIN；
- immediate：G_SEXT_INREG、G_INSERT/G_EXTRACT 的 offset。

每个 opcode 家族至少一条，重点看 `TypeIdx != 0` 和 `NewType` 非空的路径。

### 5.4 subtarget 矩阵

- `x86-64` baseline、`v2`、`v3`、`v4`；
- `-mattr=-sse2`；
- `+avx512f`、`+avx512bw`、`+avx512vl`；
- softfloat / x87 相关组合；
- `i386`（表不安装）；
- `x32`（`is64Bit()` 为真但非 LP64，表安装，`p0:32` 查询必须回退）。

再加一个混合模块：同一个 `.ll` 里不同函数带不同的 `target-features`
属性，验证 per-subtarget 表实例不会串味。

## 6. L3：对抗与变异

每个守卫都要有一个能变红的负控：

| 变异 | 期望变红的测试 |
|---|---|
| 键漏掉 slot 0 | domain differential |
| 键漏掉 slot 1 | two-slot differential |
| mem 轴从 class 改成 size | mem class test |
| 跳过地址空间检查 | `table-addrspace.ll` |
| 跳过 pointer-vector 检查 | outside witness |
| 跳过 memdesc count 检查 | outside witness |
| cell 缓存答案而不是规则号 | extended LLT fiber / KIND test |
| 比较用 `LLT::operator==` 而不是 `sameAnswer` | fiber spelling test |
| 用错代表元 fill | selfCheck mismatch |
| `Local` 非单射 | `Local` injectivity unit test |
| immediate 被错误拒绝/错误回答 | immediate test |
| feature 被塞进键或跨 subtarget 复用 | mixed-subtarget test |
| `findRule` 用最后匹配而不是 first-match | golden MIR / chain unit test |

## 7. L4：语料与模糊

`llvm/utils/gisel-table-bench/gen_ir.py` 已覆盖标量、非 2 的幂位宽、
div/rem、比较、位操作、整数/浮点转换、向量、load/store、volatile、
非对齐、指针/控制流。建议同时用于正确性和性能：

```bash
llc -global-isel -O0 -stop-after=legalizer -x86-gisel-table-check  corpus.ll
llc -global-isel -O0 -stop-after=legalizer -x86-gisel-table=false  corpus.ll
```

随机查询测试建议放在 C++ unit 里，直接构造 `LegalityQuery`：

- 随机 LLT kind / 位宽 / lane 数；
- 指针地址空间；
- memdesc 的 memory type、alignment、atomic ordering；
- immediates；
- 1~2 个类型槽；
- subtarget 配置。

对每个查询比较表与链，一秒内可跑几十万条。

## 8. L5：性能与覆盖率

性能：

- lookup 层：table 必须显著快于 chain；
- pass 层：`-time-passes` 的 legalizer 时间不能回退超过阈值；
- `once` 负控必须与 chain 接近。

覆盖率：

- `points`、`hits`、`rule_mean`、`rule_max`；
- `outside`、`splits`、`declined`、`untabulated`；
- 每张表的命中次数；
- `fills` 数量；
- 生成 `.inc` 的大小 / cell 总数。

门禁：

- 每张表至少一次命中；
- `hit / queries` 不低于基线；
- `splits` 不突然升高（键变粗的信号）；
- `outside` 不突然升高（类集变窄的信号）；
- `untabulated` 清单不漂移。

## 9. 测试矩阵

| 维度 | 取值 | 重点 |
|---|---|---|
| Target | x86_64, i386, x32, AArch64 | 安装条件、PointerBits |
| Subtarget | baseline, v2, v3, v4, -sse2, +avx512, softfloat/x87 | feature 不进键 |
| LLT | sN, iN, fN, vL sN, vscale, p0, pN, vL p0 | class/fiber/refusal |
| Query | types-only, mem, immediate, zero-type, 2-slot, variadic | key 轴 |
| Answer | Legal, widen/narrow, more/fewer, custom/lower/libcall, unsupported | 答案形状 |
| Path | Hit, Split, Outside, PointerVector, Declined, Untabulated | 回退 |
| Source | domain rep, fiber, outside witness, real IR, random | 覆盖 |

## 10. 最小可行测试集（P0）

1. 修掉当前 WIP 的 red tests：`cached`、`-x86-gisel-table-cache-extended`。
2. 表 on/off MIR diff，覆盖 `gen_ir.py` 语料。
3. domain differential + outside witnesses，作为 lit 门禁。
4. extended LLT fiber / 拼写测试。
5. AArch64 的基本 `-aarch64-gisel-table-verify` 测试（目前没有 lit 级自检）。
6. 生成器负例：未知 leaf、opaque、unkeyable、escape、3-slot、Local 契约。
7. 每张表至少一次命中的覆盖率报告。
8. `once` 负控 + benchmark 阈值。
9. 变异表：至少覆盖键漏槽、答案缓存、地址空间、mem 轴、`operator==`。
10. x32 / 32-bit / 混合 subtarget 测试。

## 11. 最终判断标准

一个合格的回归测试集应该能回答：

- 如果我把键改粗一格，哪个测试红？
- 如果我把答案缓存加回来，哪个测试红？
- 如果我把 `sameAnswer` 换成 `LLT::operator==`，哪个测试红？
- 如果我把 mem 轴从 class 改成 size，哪个测试红？
- 如果我把 `-x86-gisel-table` 默认关掉，哪个测试红？
- 如果我把某张表整个删掉，哪个测试红？
- 如果我把 `Local` 压成非单射，哪个测试红？
- 如果我只保留“表 vs 链”测试，而链本身被改错，哪个测试红？

如果这些问题都能指出具体测试文件，测试集就设计到位了。
