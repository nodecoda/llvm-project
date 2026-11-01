# Legalizer 声明语言（研究产物）

> 本目录是本地下游研究，不随 `[GlobalISel] …` 的上游补丁提交。

## 这里放什么

| 路径 | 内容 |
| --- | --- |
| `legalize-decl.v6.y` | 文法（bison，`-Wall` 0/0/0） |
| `README.md` | 本文件：布局、入口、当前版本 |

## 当前状态（2026-11-01）

- 文法：v6。相对 v5 的三处改动**全是删除**（shapes/atoms、三份括号规则、`tied`）。
- 语义与测量文档仍在 `/home/ubuntu/llvm/llvm-dev`（提交 `1457619`，D-74）：
  `2026-11-01-legalize-semantics.md`、`2026-11-01-default-inference.md`、
  `2026-11-01-grammar-review.md`、`2026-11-01-x86.decl`。
- 迁移是**移动**，不是复制：下一轮把文法与工具一起搬过来，`llvm-dev` 里只留指针。

## 入口（迁移完成后）

```
python3 tools/lint_decl.py  <file>.decl     # 用真 parser 检查文法
python3 tools/check_decl.py <file>.decl     # 语义检查（唯一实现）
python3 tools/infer_diff.py                 # 与真链对拍
```
