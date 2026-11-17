# 第 32 章：跨后端 status 矩阵——一张表看完跨后端投影

## 目标

第 29–31 章各自有一张“RVMin `.td` 用法对照”表，但它们是**按后端**
分开的。读的时候要在几张表之间来回跳，才能回答一个最实际的问题：

> 我改这一行 `let`，到底会影响哪些后端？

本章把三张表合并成一张**跨后端 status 矩阵**：

- 行：`RVMinRegisterInfo.td` / `RVMin.td` 里的每一处 `let`/`field`；
- 列：`register-info` / `instr-info` / `asm-writer` / `asm-matcher` /
  `dag-isel` / `fast-isel` / `disassembler`（后四列分别由第 34、36、37、
  38 章加入，矩阵自动扩展）；
- 单元格：该字段在这个后端里的投影状态。

本章产出：

- `dsl-surface.py` 新增 `--status-matrix`；
- 同一个模式支持 `--check` / `--update`，所以矩阵本身也是可漂移文档；
- 一张 13 行 × 7 后端的对照表，直接展示“同一行 `.td`，不同命运”。

非目标：

- 不新增后端；
- 不改变 `--usage` 的单后端行为；
- 不把 status 矩阵做成通用 BI 工具；它只回答“哪个后端读这个字段”。

## 背景：从“后端视图”到“投影视图”

前三章的表都是**后端视图**：

```text
register-info 视角：这个字段我读不读？
instr-info    视角：这个字段我读不读？
asm-writer    视角：这个字段我读不读？
```

本章要的是**投影视图**：

```text
这个字段：register-info 怎么处理？instr-info 怎么处理？asm-writer 怎么处理？
```

两者用的是同一份契约数据，只是聚合方向相反。实现上不需要新的语义分析，
只需要：

1. 把 `.td` 里的 `let`/`field` 原始用法抽取一次；
2. 对每个后端查一次 `(field -> stage)`；
3. 把结果排成矩阵。

### 单元格词汇

| 单元格 | 含义 |
| --- | --- |
| `derive` | 只被共享派生层读 |
| `project` | 只被该后端 emitter 读 |
| `derive+project` | 共享层和 emitter 都读 |
| `未消费` | 字段在该后端 schema 里，但没有读取点 |
| `—` | 字段不在该后端的 root/ambient schema 里 |
| `自定义` | 目标自定义 `field`，不属于通用 schema |

`—` 和 `未消费` 必须区分：前者是“这个后端根本看不到这个字段”，后者是
“看得到，但这次投影不用它”。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `llvm/utils/TableGen/dsl-surface.py` | `collect_td_usages` / `collect_status_rows` / `render_status_matrix` |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td` | 寄存器 schema 的 RVMin 用法 |
| `llvm/lib/Target/RVMin/RVMin.td` | 目标级配置的 RVMin 用法 |
| `llvm/lib/Target/RVMin/docs/32-cross-backend-status.md` | 本章；一个生成块 |

## 关键实现：把三张表压成一张

### 1. 原始用法只抽一次

`collect_td_usages()` 不关心后端，只返回：

```python
(file, line, field, is_custom_field)
```

这样 `--usage` 和 `--status-matrix` 共享同一套 `.td` 解析，不会出现
“两个模式对同一行给出不同行号”的问题。

### 2. 每个后端查一次 stage

`collect_status_rows()` 对每个后端建 `field -> contract row` 索引，然后：

```python
if custom:          cell = "自定义"
elif field not in schema: cell = "—"
elif row["sites"]:  cell = row["stage"]
else:               cell = "未消费"
```

### 3. 汇总数字

块末给出三个计数：

- 所有后端都消费的用法数；
- 至少一个后端消费的用法数；
- 自定义字段数。

这三个数字比单看某一行更能说明“跨后端共享”的程度。

### 4. CLI

```bash
# 只看矩阵
python3 dsl-surface.py --status-matrix --usage a.td b.td

# 校验文档里的矩阵块
python3 dsl-surface.py --status-matrix --check DOC --usage a.td b.td

# 重新生成矩阵块
python3 dsl-surface.py --status-matrix --update DOC --usage a.td b.td
```

`--status-matrix` 与 `--check`/`--update` 一起使用时，只处理矩阵块，
不会碰单后端的 contract/matrix/derivations/usage 块。这样第 32 章可以只放
一张表，不必复制前三章的内容。

## 跨后端 status 矩阵

下面这一段由脚本生成，不要手改；改 `.td` 或后端配置后运行 `--update`。

<!-- BEGIN: GENERATED CROSS-BACKEND STATUS -->

同一份 `.td` 在每个后端下的投影状态；`—` 表示该字段不在这个后端的 schema 里。

| 文件:行 | 字段 | 声明于 | `register-info` | `instr-info` | `asm-writer` | `asm-matcher` | `dag-isel` | `fast-isel` | `disassembler` |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:12` | `FallbackRegAltNameIndex` | `RegAltNameIndex` | 未消费 | — | `project` | — | — | — | — |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:17` | `Namespace` | `Instruction`, `RegAltNameIndex`, `Register`, `RegisterClass`, `SubRegIndex` | `derive+project` | `derive` | `derive+project` | `derive+project` | `derive` | `derive+project` | `derive` |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:18` | `HWEncoding` | `Register` | `project` | — | 未消费 | 未消费 | 未消费 | 未消费 | 未消费 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:19` | `AltNames` | `Register` | 未消费 | — | `project` | `project` | 未消费 | 未消费 | 未消费 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:20` | `DwarfNumbers` | `Register` | `project` | — | 未消费 | 未消费 | 未消费 | 未消费 | 未消费 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:25` | `RegAltNameIndices` | `Register` | 未消费 | — | `project` | `project` | 未消费 | 未消费 | 未消费 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:79` | `DecoderMethod` | `DAGOperand`, `InstructionEncoding` | 未消费 | 未消费 | 未消费 | 未消费 | 未消费 | 未消费 | 未消费 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:107` | `Namespace` | `Instruction`, `RegAltNameIndex`, `Register`, `RegisterClass`, `SubRegIndex` | `derive+project` | `derive` | `derive+project` | `derive+project` | `derive` | `derive+project` | `derive` |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:108` | `HWEncoding` | `Register` | `project` | — | 未消费 | 未消费 | 未消费 | 未消费 | 未消费 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:109` | `AltNames` | `Register` | 未消费 | — | `project` | `project` | 未消费 | 未消费 | 未消费 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:110` | `DwarfNumbers` | `Register` | `project` | — | 未消费 | 未消费 | 未消费 | 未消费 | 未消费 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:113` | `RegAltNameIndices` | `Register` | 未消费 | — | `project` | `project` | 未消费 | 未消费 | 未消费 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:168` | `DecoderMethod` | `DAGOperand`, `InstructionEncoding` | 未消费 | 未消费 | 未消费 | 未消费 | 未消费 | 未消费 | 未消费 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:179` | `DecoderMethod` | `DAGOperand`, `InstructionEncoding` | 未消费 | 未消费 | 未消费 | 未消费 | 未消费 | 未消费 | 未消费 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:186` | `DecoderMethod` | `DAGOperand`, `InstructionEncoding` | 未消费 | 未消费 | 未消费 | 未消费 | 未消费 | 未消费 | 未消费 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:191` | `DecoderMethod` | `DAGOperand`, `InstructionEncoding` | 未消费 | 未消费 | 未消费 | 未消费 | 未消费 | 未消费 | 未消费 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:16` | `Num` | — | 自定义 | 自定义 | 自定义 | 自定义 | 自定义 | 自定义 | 自定义 |
| `llvm/lib/Target/RVMin/RVMinRegisterInfo.td:106` | `Num` | — | 自定义 | 自定义 | 自定义 | 自定义 | 自定义 | 自定义 | 自定义 |
| `llvm/lib/Target/RVMin/RVMin.td:148` | `AsmWriterClassName` | `AsmWriter` | — | — | `project` | — | — | — | — |
| `llvm/lib/Target/RVMin/RVMin.td:155` | `ShouldEmitMatchRegisterAltName` | `AsmParser` | — | — | — | `project` | — | — | — |
| `llvm/lib/Target/RVMin/RVMin.td:156` | `AllowDuplicateRegisterNames` | `AsmParser` | — | — | — | `project` | — | — | — |
| `llvm/lib/Target/RVMin/RVMin.td:164` | `InstructionSet` | — | — | — | — | — | — | — | — |
| `llvm/lib/Target/RVMin/RVMin.td:165` | `AssemblyParsers` | `Target` | — | — | — | `derive` | — | — | — |
| `llvm/lib/Target/RVMin/RVMin.td:166` | `AssemblyWriters` | `Target` | — | — | `derive` | — | — | — | — |

共 24 处使用；7 个后端都消费 2 处，至少一个消费 16 处，自定义字段 2 处。
<!-- END: GENERATED CROSS-BACKEND STATUS -->

## 深读：这张表告诉了我们什么

### 1. `AltNames`：两列空、两列满、两列未消费

`AltNames` 在 `register-info` / `instr-info` 下是 `—`，在 `asm-writer` /
`asm-matcher` 下都是 `project`，在 `dag-isel` / `fast-isel` 下是
`未消费`。这不是“字段写错了”，而是它只属于汇编打印投影。第 29 章把它标成
“asm writer / asm matcher（无效）”时，说的是 register-info 视角；本章把它
放回全局，就能看到它在 asm-writer / asm-matcher 里都是核心输入。

### 2. `DwarfNumbers` / `HWEncoding`：反向案例

它们在 `register-info` 下是 `project`，在 `asm-writer` / `asm-matcher`
下是 `未消费`，`dag-isel` / `fast-isel` 也不读；在 `instr-info` 下是
`—`。同一行 `.td`，在七个后端里分别是“核心输入”“看得到但不用”
“根本看不到”。

### 3. `Namespace`：唯一七列都消费的字段

`Namespace` 在七个后端里都是 `derive` 或 `derive+project`。这是真正的共享
字段：指令、寄存器、汇编打印都要 namespace。跨后端矩阵里，这种行最适合
作为“改之前必须七处都验证”的候选。

### 4. `Num`：七列都是“自定义”

`Num` 是 RVMin 自己在 `.td` 里声明的 `field`，不属于 `Target.td` 的通用
schema。七个后端都不认识它；只有读它的 C++ 才需要保证语义。矩阵把
“自定义字段”单独标出来，避免被误认为“未消费”。

### 5. `AssemblyWriters`：ambient 字段

`AssemblyWriters` 在 asm-writer 下是 `derive`，因为它被
`CodeGenTarget::getAsmWriter()` 读取。它不是任何根类的字段，而是
`Target` 记录上的 ambient 配置。asm-matcher、dag-isel、fast-isel 与
disassembler 都没有把它纳入 schema（匹配器/解码器不使用打印器配置），
所以只亮一列，正好说明
“目标级字段”和“记录类字段”的区别。

### 6. `ShouldEmitMatchRegisterAltName` / `AllowDuplicateRegisterNames`：新增一列后才亮

这两行是 asm-matcher 的输入。第 32 章首次发布时，矩阵只有三个后端，
两行都是 `—`；第 34 章把 asm-matcher 加进 `BACKENDS` 后，它们自动变成
`project`。第 36 章再加入 dag-isel、第 37 章加入 fast-isel、第 38 章加入
disassembler 后，这两行仍只亮 asm-matcher 一列。这正好演示了**后端集合是
SSOT**：文档不需要手改逻辑，只需重新 `--update`。

### 7. 汇总数字怎么读

“7 个后端都消费 1 处，至少一个消费 11 处”说明：13 处用法里只有 1 处是真正
七后端共享的（`Namespace`），但有 11 处至少被一个后端消费。剩下 1 处是
`InstructionSet`（见常见坑 3），另有 1 处是自定义字段（`Num`）。

### 8. 为什么不做成“字段 × 所有 43 个后端”

矩阵的列是 `BACKENDS` 里显式配置的后端。自动扫描所有 `gen-*` 字符串能得到
“谁读了这个字段名”，但无法给出 stage、默认值、语义注解。本章的矩阵是
**契约级**视图，不是 grep 级视图。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
T="$W/llvm/utils/TableGen/dsl-surface.py"
DOC="$W/llvm/lib/Target/RVMin/docs/32-cross-backend-status.md"
TD=("$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td"
    "$W/llvm/lib/Target/RVMin/RVMin.td")

# 1. 只看矩阵
python3 "$T" --status-matrix --usage "${TD[@]}"

# 2. 文档块校验
python3 "$T" --status-matrix --check "$DOC" --usage "${TD[@]}"

# 3. 行号漂移后重新生成
python3 "$T" --status-matrix --update "$DOC" --usage "${TD[@]}"

# 4. 前三章回归
python3 "$T" --check "$W/llvm/lib/Target/RVMin/docs/29-register-info-dsl.md" \
  --usage "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td" \
          "$W/llvm/lib/Target/RVMin/RVMinCallingConv.td"
python3 "$T" --backend instr-info --check \
  "$W/llvm/lib/Target/RVMin/docs/30-instr-info-dsl.md" \
  --usage "$W/llvm/lib/Target/RVMin/RVMin.td"
python3 "$T" --backend asm-writer --check \
  "$W/llvm/lib/Target/RVMin/docs/31-asm-writer-dsl.md" \
  --usage "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td"

# 5. 全量门禁
"$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"
```

## 预期输出

```text
$ python3 dsl-surface.py --status-matrix --check doc32.md --usage ...
dsl-surface: OK (cross-backend status: 13 usages, 7 backends)

$ python3 dsl-surface.py --status-matrix --usage ...
| 文件:行 | 字段 | 声明于 | register-info | instr-info | asm-writer |
...
共 13 处使用；7 个后端都消费 1 处，至少一个消费 11 处，自定义字段 1 处。

$ scripts/rvmin-verify-all.sh "$B/bin"
...
ALL GATES PASS (run rvmin-run.sh separately for the QEMU check)
```

## 常见坑

1. **`--status-matrix` 必须配 `--usage`。** 没有 `.td` 输入就没有行；脚本
   会直接报错，而不是输出空表。
2. **`--status-matrix --update` 只更新矩阵块。** 它不会碰 contract/matrix/
   derivations/usage 块，适合“只放一张表”的章节。
3. **`—` 不等于“未消费”。** `—` 表示字段不在该后端 schema；`未消费` 表示
   在 schema 但没有读取点。混用会误导读者。
4. **ambient 字段要显式登记。** `AssemblyWriters` 只有加进 asm-writer 的
   `ambient_fields` 才会出现在该列；否则会被误标成 `—`。
5. **矩阵列顺序就是 `BACKENDS` 的插入顺序。** 新增后端会自动多一列，
   但列名和顺序会变，文档需要 `--update`。
6. **不要拿全 `—` 判断字段无用。** 它可能属于尚未纳入的 dag-isel /
   dag-isel 等后端。
7. **status 矩阵不替代单后端契约。** 它只给 stage 速览；默认值、类型、
   读取点、语义仍在各后端的 contract 块里。

## 练习

1. 把 `RVMinInstrInfo.td` 也加进 `--usage`，看矩阵会膨胀到多少行；先按
   `(field, status)` 聚合再决定是否适合放进文档。
2. 给矩阵增加一列“后端消费计数”，或增加一个 `--status-summary` 模式，
   只输出汇总数字。
3. 把 `asm-matcher` 加进 `BACKENDS`，观察
   `ShouldEmitMatchRegisterAltName` / `AllowDuplicateRegisterNames` 两行
   从全空变成有值。
4. 设计一个“影响面”命令：输入字段名，输出所有读它的后端与读取点。
   复用 `scan_matrix()` 的数据即可。
5. 把 status 矩阵扩展到 `llvm/docs/TableGen/BackEnds.md`，需要解决哪些
   后端选择与注解维护问题？

## 产物清单

| 文件 | 变更 |
| --- | --- |
| `llvm/utils/TableGen/dsl-surface.py` | 新增 `--status-matrix`；原始 `.td` 用法抽取与跨后端映射；asm-writer 增加 `AssemblyWriters` ambient 字段 |
| `llvm/lib/Target/RVMin/docs/32-cross-backend-status.md` | 新增：本章（一个生成块） |
| `llvm/lib/Target/RVMin/docs/README.md` | 新增第 32 章索引行 |
| `llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh` | 第 7 项增加 status 矩阵校验 |
