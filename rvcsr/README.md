# rvcsr：RISCVSystemOperands 的领域编译器（独立教学后端）

`rvcsr` 把 `RISCVSystemOperands.td` 这一“CSR 名字绑定”领域从 TableGen 里拿出来，
用一门**非图灵完备的声明式 DSL** 描述，再由一个独立 C++ 编译器直接生成
`RISCVGenSearchableTables.inc`（兼容模式）或自带最小 runtime 的独立表（standalone 模式）。

- 不链接 LLVM，不 include LLVM 头，不依赖 TableGen；
- 只做数据：`csr` / `alias` / `deprecated` / `vendor` / `requires`；
- 只有声明、有界宏、整数算术；没有函数、递归、通用控制流。

本目录目前只含 **v1 语法规范**（2026-11-11 frozen），实现按计划逐步落地。

## 目录

| 路径 | 内容 |
| --- | --- |
| [`docs/grammar-v1.md`](docs/grammar-v1.md) | 人类可读语法 SSOT：词法、EBNF、静态语义 E/W 规则、冻结规则、终止性论证 |
| [`grammar/rvcsr.ebnf`](grammar/rvcsr.ebnf) | 机器可读 EBNF（parser 对照用） |
| [`examples/riscv/riscvcsrs.rvcsr`](examples/riscv/riscvcsrs.rvcsr) | 覆盖全部核心产生式的冒烟样例 |
| [`examples/riscv/riscvfeatures.rvfeat`](examples/riscv/riscvfeatures.rvfeat) | feature 声明样例（含 `extern feature`） |

## 语法速览

```text
csr fflags = 0x001
alias mcontrol of 0x7a1
deprecated sbadaddr of 0x143

for i in 3..31 {
  csr hpmcounter${i}    = 0xc03 + (i - 3)
  csr hpmcounter${i}h   = 0xc83 + (i - 3) { rv32only }
}

vendor qc requires Xqciint {
  for i in 0..31 {
    csr mclicilvl${i:2} = 0xbc0 + i
  }
}
```

三条最容易被写错的规则：

1. **源文件里没有 `.`**：`sf.` / `qc.` / `aif.` 是投影产物，由 `vendor` 作用域生成。
2. **模板只有 `${i}` / `${i:width}`**：`${i:2}` 是最小宽度 2、补零；
   没有 `printf` 式格式化。
3. **alias / deprecated 只能顶层、无属性**：它们继承 canonical 的可用性。

## 版本与来源

- 语法版本：v1（2026-11-11），状态 frozen；
- 上游 SSOT：`/home/ubuntu/admin/ana-docs/rvcsr-grammar-v1-2026-11-11.md`；
- 实现计划：`.omx/plans/2026-11-11-rvcsr-standalone-dsl.md`；
- 改动规则：先改 `docs/grammar-v1.md` 与 `grammar/rvcsr.ebnf`，再改 parser；
  parser 不允许支持语法文件之外的东西。
