# Legalizer 声明语言（研究产物）

> 本目录是本地下游研究，不随 `[GlobalISel] …` 的上游补丁提交。

## 这里放什么

| 路径 | 内容 |
| --- | --- |
| `legalize-decl.v6.y` | 文法（bison，`-Wall` 0/0/0） |
| `README.md` | 本文件：布局、入口、当前版本 |

## 当前状态（2026-11-01）

- 文法：v6 **活文件**，只做加法，当前 v6.1（`shapes on 0;`）；不兼容改写才换名。
- 5 个语言缺口全部关闭（G3 `252f28c`、G4 `aa383ae`、G1 `11125c7`、G2 `195917a`、
  G5 `e52e623`）。门禁唯一清单：`/home/ubuntu/llvm/llvm-dev/GATES.md`。
- 全语料默认推断：63 个规则集，75900/80610（94.2%），43 个完全复现；槽 0 子集
  3240/3240 未动；`not measured` 34 → 0。
- 语义与测量文档仍在 `/home/ubuntu/llvm/llvm-dev`：
  `2026-11-01-legalize-semantics.md`、`2026-11-01-default-inference.md`（§5.6 多槽）、
  `2026-11-01-grammar-review.md`、`2026-11-01-x86.decl`。
- 迁移是**移动**，不是复制：只有把工具一起搬过来、`llvm-dev` 留指针之后，
  上面这些路径才会失效。**还没做**——工具仍以 llvm-dev 为家。

## 入口（迁移完成后）

```
python3 tools/lint_decl.py  <file>.decl     # 用真 parser 检查文法
python3 tools/check_decl.py <file>.decl     # 语义检查（唯一实现）
python3 tools/infer_diff.py                 # 与真链对拍
```
