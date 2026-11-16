# 第 35 章：把契约前移——从教程到上游文档

## 目标

第 29–34 章把“后端读哪些字段”做成了可执行的契约，但它只存在于两个地方：

- 代码：`llvm/utils/TableGen/dsl-surface.py`；
- 教程：`llvm/lib/Target/RVMin/docs/29–34`。

上游读者不会去 RVMin 目录找 TableGen 的通用方法。本章把**方法论**提炼进
`llvm/docs/TableGen/BackEnds.md`：

> 在“后端目录”之前，加一节 `Backend field contracts`，说明字段即 DSL、
> `(declaring class, field)` 主键、四种投影 stage、提取命令、以及新增后端的
> 扩展点。

本章产出：

- `BackEnds.md` 新增 96 行的通用章节；
- 第 35 章记录“哪些内容前移、哪些刻意留在生成件里”；
- 门禁增加一条上游文档一致性检查（标题、锚点、工具路径）。

非目标：

- 不把 RVMin 的字段统计和中文注解搬进上游文档；
- 不把 `dsl-surface.py` 改写成上游可合并的最终形态（需要英文注解与测试）；
- 不修改任何后端行为。

## 背景：为什么文档层是最后一步

前三层已经各自成立：

```text
schema（.td） → 提取器（dsl-surface.py） → 生成件（各章文档块）
```

缺的是**入口**。`BackEnds.md` 的 Introduction 里本来就有这句话：

> There is an implicit contract between the TableGen files, the back-ends and
> their users.

本章就是把这句“implicit”展开成可执行的模型。位置也有讲究：

- 放在 `## LLVM BackEnds` 之前：它描述所有后端共有的输入面，而不是某一个
  后端的用途；
- 不放在 `BackGuide.md`：BackGuide 讲“怎么写后端”，本章讲“怎么读后端已经
  写死的输入面”，两者互补。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `llvm/docs/TableGen/BackEnds.md` | **本章改造**：新增 `Backend field contracts` 节 |
| `llvm/docs/TableGen/BackGuide.md` | 后端开发指南；本章只建立分工，不修改 |
| `llvm/utils/TableGen/dsl-surface.py` | 提取器；文档中的命令都指向它 |
| `llvm/lib/Target/RVMin/docs/35-upstream-contract.md` | 本章 |

## 关键实现：这一节写了什么

### 1. 六个小节

| 小节 | 回答的问题 |
| --- | --- |
| `Why the field names are the DSL` | 为什么 `getValueAsInt("CostPerUse")` 里的字符串就是规格 |
| `The (declaring class, field) key` | 为什么不能只按字段名建索引 |
| `Four projection stages` | `schema` / `derive` / `project` / `derive+project` 各是什么 |
| `Extracting a contract` | 三条命令：单后端、跨后端矩阵、字段影响面 |
| `Adding a backend to the extractor` | `sources` / `td_files` / `roots_from` / `extra_roots` / helper 的职责 |
| `Scope` | 它只描述“读什么”，不描述“生成代码是什么” |

stage 表是这一节的核心：

```text
schema          声明了但本后端不读
derive          只被共享 C++ 读（建模/改写阶段）
project         只被本后端 emitter 读（直接发射）
derive+project  两者都读
```

### 2. 三条命令与教程一一对应

```bash
# 第 29–31、34 章：单后端契约
llvm/utils/TableGen/dsl-surface.py --backend asm-matcher

# 第 32 章：跨后端矩阵
llvm/utils/TableGen/dsl-surface.py --status-matrix --usage Target.td

# 第 33 章：字段影响面
llvm/utils/TableGen/dsl-surface.py --effect TSFlags
```

文档里只写命令和语义，不写“RVMin 有 13 处使用”这类数字。数字属于生成件，
会随 `.td` 和 LLVM 版本漂移。

### 3. 刻意留在生成件里的内容

| 内容 | 留在哪里 | 原因 |
| --- | --- | --- |
| 166/142/42 等统计 | 各章生成块 | 随版本漂移 |
| 中文语义注解 | `dsl-surface.py` + RVMin 文档 | 上游脚本需要先做英文 pass |
| 逐后端字段表 | RVMin 29–34 章 | 属于示例，不是通用规范 |
| `--check` 的文档块协议 | RVMin 门禁 | 依赖具体文档布局 |

### 4. 上游化还差什么

本章只前移**方法论**。要让 `dsl-surface.py` 本身可上游合并，至少还差：

1. 注解与报错信息英文化；
2. 为提取器增加 lit 测试（当前只有 RVMin 门禁在跑它）；
3. `Backend` 配置从脚本常量改成可注册的表；
4. 与 `BackGuide.md` 建立双向链接；
5. 按 LLVM review 节奏拆成“文档 / 工具 / 测试”三个提交。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

# 1. 章节与锚点存在
grep -n "^## Backend field contracts" "$W/llvm/docs/TableGen/BackEnds.md"
grep -n "^### Extracting a contract" "$W/llvm/docs/TableGen/BackEnds.md"

# 2. 文档引用的路径存在
test -x "$W/llvm/utils/TableGen/dsl-surface.py"

# 3. 文档里的命令可直接运行
"$W/llvm/utils/TableGen/dsl-surface.py" --backend asm-matcher >/dev/null
"$W/llvm/utils/TableGen/dsl-surface.py" --effect TSFlags >/dev/null

# 4. 全量门禁
"$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"
```

本环境未安装 Sphinx，因此不构建 `docs-llvm-html`；章节结构用静态检查覆盖。
上游提交前应补一次 `ninja docs-llvm-html`。

## 预期输出

```text
$ grep -n "^## Backend field contracts" llvm/docs/TableGen/BackEnds.md
29:## Backend field contracts

$ grep -n "^### Extracting a contract" llvm/docs/TableGen/BackEnds.md
77:### Extracting a contract

$ scripts/rvmin-verify-all.sh "$B/bin"
...
PASS 7/7: TableGen DSL contracts + status matrix + field effect
ALL GATES PASS (run rvmin-run.sh separately for the QEMU check)
```

## 常见坑

1. **不要把生成数字写进上游文档。** `166/142/42` 是当前脚本配置的输出，
   不是 TableGen 的规范。
2. **文档里的命令必须能在仓库根目录直接跑。** `dsl-surface.py` 默认识别
   LLVM 根目录，不再接受教程里的相对路径。
3. **`include` 不跟随。** 文档明确写了 `td_files` 要列全；这是
   `parse_td()` 的实现边界，不是笔误。
4. **`BackEnds.md` 是目录，不是教程。** 方法论放在 `## LLVM BackEnds`
   之前；某个后端的细节仍留在它的 `###` 小节里。
5. **文档先行不等于工具可上游。** 脚本里的中文注解、缺少 lit 测试都是
   合并阻塞项，本章不掩盖它们。

## 练习

1. 把 `BackGuide.md` 的 “Getting Record Names and Fields” 小节加一条
   指向 `Backend field contracts` 的链接，需要改几处？
2. 给 `dsl-surface.py` 增加一个 `--list-backends`，让文档不必硬编码五个
   名字；输出应该包含哪些字段？
3. 用 `--json` 输出生成一份“字段 → reader”索引，比较它和
   `--effect` 的差异。
4. 如果把 stage 从四值改成“读/不读”两值，会丢失哪些信息？用
   `TSFlags` 和 `AltNames` 举例。
5. 为 `BackEnds.md` 的新节写一个 markdown lint 规则（标题层级、代码块
   语言、相对链接），最少需要哪几条？

## 产物清单

| 文件 | 变更 |
| --- | --- |
| `llvm/docs/TableGen/BackEnds.md` | 新增 `Backend field contracts` 节（96 行） |
| `llvm/lib/Target/RVMin/docs/35-upstream-contract.md` | 新增：本章 |
| `llvm/lib/Target/RVMin/docs/README.md` | 新增第 35 章索引行；范围改为 29–35 |
| `llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh` | 第 7 项增加上游文档锚点/路径检查 |
