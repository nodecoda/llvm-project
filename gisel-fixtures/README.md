# gisel fixtures

六篇《GlobalISel 合法化规则的可测试性》里出现的可复现输入。**不参与 `check-llvm`**：
目录在仓库根，不在 `llvm/test` 下，lit 配置是独立的；上游不需要这些文件。

> 分支状态：文章引用的是 frozen tag `gisel-legalizer-rules-2026-11-07`（9 条用例）。
> 本分支 `test/gisel-fixtures-axes` 从那个 tag 长出来，是扩展实验区，**不进文章**。

## 依赖

- 一个 **assertions** 构建的 `llc`，targets 含 X86 / AArch64 / RISCV
  （`-debug-only=legalizer-info` 只在 asserts 构建里可用）；
- 同一构建里的 `FileCheck`、`not`；
- `lit`：`python3 llvm/utils/lit/lit.py`（`llvm-lit` 不是 ninja 目标）。

## 跑

```bash
# 一条命令：跑完 9 个用例，并把 llc / 提交号记进 _out/snapshot.txt
bash gisel-fixtures/run.sh /home/ubuntu/llvm/builds/<task>/bin/llc

# 分开跑也一样
B=/home/ubuntu/llvm/builds/<task>/bin
python3 llvm/utils/lit/lit.py -sv \
  --param llc="$B/llc" --param filecheck="$B/FileCheck" gisel-fixtures

# 挂死用例单独复核：默认 60 秒，lit 里用 20 秒
bash gisel-fixtures/cases/05-hang/check.sh "$B/llc"
```

## 用例与文章

| 用例 | 文章 | 断言 |
|---|---|---|
| `01-chain/add-v2i8.ll` | 第 1 篇 §1 | `<2 x s8>` → `MoreElements` → `<16 x s8>` → `Legal` |
| `01-chain/add-i24.ll` | 第 1 篇 §1 | `s24` → `WidenScalar` → `s32` → `Legal` |
| `01-chain/add-v3i24.ll` | 第 1 篇 §1 | 两刀：`<3 x s24>` → `s24`（scalarize）→ `s32` |
| `02-boundary/add-v15i8.ll` | 第 2 篇 §4.3 | 15 lane 要补到 16 |
| `02-boundary/add-v16i8.ll` | 第 2 篇 §4.3 | 16 lane 一次命中，没有任何动作 |
| `02-boundary/add-v32i8.ll` | 第 2 篇 §4.3 | 32 lane 拆成两条 16 lane 的 `G_ADD` |
| `02-axes/config-avx2.ll` | 第 2 篇 §1 | 同一个 `<32 x i8>`：SSE2 拆成两条，`+avx2` 一次命中 |
| `02-axes/imm-sext.ll` | 第 2 篇 §1 | `Tys` 全是 `{s64, }`，imm 8/16/24/32 给出三类答案 |
| `02-axes/addrspace.ll` | 第 2 篇 §1 | 同样的 `s32` load：`p0` Legal，`p272` unable to legalize |
| `05-hang/mul-v2i32.ll` | 第 5 篇 §0 | GlobalISel 不返回（#236649）；SelectionDAG 返回 0 |
| `06-cross/add-i32.ll` | 第 6 篇 §1 | 标量 `i32`：X86 `Legal` / AArch64 `Legal` / RISCV `Custom` |
| `06-cross/add-v4i32.ll` | 第 6 篇 §1 | 向量：X86、AArch64 `Legal` / RISCV `unable to legalize` |

## 纪律

- 输入只来自上游 IR，不含公司私有 target；
- 期望输出是**跑出来的**，不是手写的；换 `llc` 后重跑，diff 要能解释；
- 断言优先看**决策**（`Legal` / `MoreElements` / `Custom`），其次才看 MIR 文本；
- `-debug-only=legalizer-info` 的文本是 llc 内部格式，跨提交可能变；这些用例是
  "某个快照上的行为账本"，不是上游契约；
- 这些不是上游测试，不提交到 `llvm/test`，也不往上游发 PR。
