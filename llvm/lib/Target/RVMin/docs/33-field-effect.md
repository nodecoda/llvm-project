# 第 33 章：影响面查询——一个字段会牵动哪些后端

## 目标

第 32 章的 status 矩阵是“按 `.td` 行看”的视图。本章补上另一个方向：

> 如果我只改一个字段（比如 `TSFlags`），到底有哪些后端、哪些 C++ 文件会受影响？

`dsl-surface.py` 新增 `--effect FIELD`，一次回答三个问题：

1. **声明**：这个字段名被哪些 `declaring class` 声明？类型/默认值是什么？
2. **投影**：在已配置的后端里，它分别停在 `derive` / `project` / `未消费` / 不存在？
3. **读取点**：扫描 `llvm/utils/TableGen/**`，哪些 reader（`-gen-*`、共享层、
   helper）真正 `getValue*("FIELD")`？

本章产出：

- `--effect FIELD` 查询模式；
- 同一模式下的 `--check` / `--update`，让影响面报告也是可漂移文档；
- 以 `TSFlags` 为固定示例的生成块，并用 `AltNames` / `Namespace` /
  `Body` / `Num` 四个字段做对照。

非目标：

- 不做 C++ 类型推导；读取点仍按字段名聚合；
- 不把 `--effect` 做成调用图；它只回答“谁按名字读了这个字段”；
- 不新增后端。

## 背景：从“行视图”到“字段视图”

前三章的契约和矩阵都是**行视图**：

```text
一行 .td  → 三个后端的 stage
```

本章要的是**字段视图**：

```text
一个字段名 → 声明类 → 各后端 stage → 所有 getValue* 读取点
```

这两者回答的问题不同：

| 视图 | 问题 | 典型命令 |
| --- | --- | --- |
| 行视图 | 我改这一行 `.td` 会影响谁？ | `--status-matrix` |
| 字段视图 | 我改这个 schema 字段会影响谁？ | `--effect FIELD` |

字段视图更适合“上游改 `Target.td`”或“我要重命名一个字段”的场景。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `llvm/utils/TableGen/dsl-surface.py` | `render_effect` / `--effect` / 生成块支持 |
| `llvm/include/llvm/Target/Target.td` | 字段声明与默认值 |
| `llvm/utils/TableGen/**` | `scan_matrix()` 扫描的 reader 全集 |
| `llvm/lib/Target/RVMin/docs/33-field-effect.md` | 本章；一个生成块 |

## 关键实现：一次查询的三层数据

### 1. 声明层：合并所有后端的 `(declaring class, field)`

每个后端的契约里已经有 `declared_in`、`types`、`defaults`。`render_effect()`
把它们按 declaring class 合并：

```text
TSFlags:
  Instruction   bits<64> / bits<8>  -> asm-writer, instr-info
  RegisterClass bits<64> / bits<8>  -> asm-writer, instr-info, register-info
```

注意“出现于后端”表示**该后端 schema 里声明了这个字段**，不代表它消费。
`TSFlags` 在 register-info 里声明于 `RegisterClass`，但 register-info 的
emitter 并不读它（只被共享寄存器模型读）。

### 2. 投影层：每个后端一行

对每个后端查 `field -> row`：

- 有读取点：显示 `stage` + 前三个读取点 + 该后端的语义注解；
- 声明了但没读取点：显示 `未消费` + 归属说明；
- 字段不在该后端 schema：显示 `—`。

### 3. 读取层：扫描所有 `llvm/utils/TableGen/**`

`scan_matrix()` 本来用于投影矩阵；`--effect` 复用它，但只查一个字段。
reader 的命名规则不变：

- `/Common/`、`/Basic/` → `shared`；
- 文件里有 `"gen-xxx"` → `xxx`；
- 否则 → `helper:<Stem>`。

这样能同时看到“契约级后端”和“原始 reader”两个粒度。

### 4. CLI 与文档块

```bash
# 直接查询
python3 dsl-surface.py --effect TSFlags

# 校验/更新文档里的影响面块
python3 dsl-surface.py --effect TSFlags --check DOC
python3 dsl-surface.py --effect TSFlags --update DOC
```

`--effect` 与 `--status-matrix` 互斥；两者都只处理自己的生成块。

## 影响面：`TSFlags`

下面这一段由脚本生成，不要手改；改 schema 或后端配置后运行 `--update`。

<!-- BEGIN: GENERATED FIELD EFFECT -->

影响面：`TSFlags`

**声明**

| declaring class | 类型 | 默认值 | 出现于后端 |
| --- | --- | --- | --- |
| `Instruction` | `bits<64>` / `bits<8>` | `0` | `asm-writer`, `instr-info` |
| `RegisterClass` | `bits<64>` / `bits<8>` | `0` | `asm-writer`, `instr-info`, `register-info` |

**投影**

| backend | stage | 读取点 | 语义 |
| --- | --- | --- | --- |
| `register-info` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:825` | bits<8>，原样写入寄存器类表。 |
| `instr-info` | `derive+project` | `llvm/utils/TableGen/InstrInfoEmitter.cpp:1415`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:825` | 目标自定义标志位；`emitRecord` 校验位宽并写入 MCInstrDesc 的 TSFlags。同名 `RegisterClass.TSFlags` 是寄存器类标志。 |
| `asm-writer` | `derive` | `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:825` | 目标自定义标志位；`emitRecord` 校验位宽并写入 MCInstrDesc 的 TSFlags。同名 `RegisterClass.TSFlags` 是寄存器类标志。 |

**所有读取点**（扫描 `llvm/utils/TableGen/**`）

| reader | 读取点 |
| --- | --- |
| `instr-info` | `llvm/utils/TableGen/InstrInfoEmitter.cpp:1415` |
| `shared` | `llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp:1932`, `llvm/utils/TableGen/Common/CodeGenRegisters.cpp:825` |

共 2 个声明类 · 3/3 个后端消费 · 2 个 reader。
<!-- END: GENERATED FIELD EFFECT -->

## 深读：五个字段的对照（外加一个易混点）

### 1. `TSFlags`：两个声明类，三个后端都“碰”

`TSFlags` 同时声明在 `Instruction` 和 `RegisterClass`：

- `Instruction.TSFlags`：目标自定义标志位，`InstrInfoEmitter` 校验位宽后写入
  MCInstrDesc；
- `RegisterClass.TSFlags`：寄存器类标志，`CodeGenRegisters` 读取。

影响面报告显示 3/3 后端消费，但 stage 不同：

- `instr-info` 是 `derive+project`（共享层 + emitter 都读）；
- `register-info` / `asm-writer` 是 `derive`（只被共享寄存器模型读）。

这也是一个注解局限的例子：语义列按字段名合并，`TSFlags` 的说明同时提到
指令和寄存器类。真要区分调用点的静态类型，需要 C++ 类型推导，不在本章范围。

### 2. `AltNames`：只声明在 `Register`，只被汇编侧消费

`AltNames` 只声明在 `Register`，但：

- `register-info`：`未消费`（字段在 schema 里，但 emitter 不读）；
- `instr-info`：`—`（`Register` 不是它的 root）；
- `asm-writer`：`project`（emitter 直接读）。

原始 reader 只有两个：`asm-writer` 和 `asm-matcher`。所以“改 `AltNames`”
的真实影响面是汇编打印/匹配，不是 register-info。第 29 章把它标成无效，
第 31 章把它标成核心输入，本章把两边合并成一句话。

### 3. `Namespace`：真正的全局字段

`Namespace` 有 5 个声明类：

- `Instruction`、`Register`、`RegisterClass`、`SubRegIndex`、`RegAltNameIndex`。

3/3 后端都消费，reader 有 8 个，`shared` 一个 reader 就有 26 个读取点。
这是本章最典型的“改之前必须全链路验证”的字段。

### 4. `Body`：只属于 instr-info 的代码注入点

`Body` 声明在 `FunctionPredicateBase`（`TIIPredicate` 的基类），只有
`instr-info` 读，stage 是 `project`。它和 `AltOrderSelect` 一样是 `code` 字段，
直接展开成目标 C++；影响面报告里只有一行，但这一行是“生成代码”的入口。

### 5. `Num`：0/0/0 也是有效答案

`Num` 是 RVMin 自定义字段，不在任何已配置后端的 schema 里：

```text
声明类 0 · 消费后端 0/3 · reader 0
```

这不是查询失败，而是正确结论：它是目标自定义字段，只有读它的 C++ 才需要
保证语义。`--effect` 用“不在已配置后端的 schema 里”明确区分“查不到”和
“没有读取点”。

### 6. reader 与 backend 不是一回事

`--effect` 同时给出两层：

- **投影层**：三个已配置后端的 stage；
- **读取层**：所有 `-gen-*` / shared / helper 的原始读取点。

例如 `Namespace` 的读取层有 `asm-matcher`、`emitter`、`fast-isel`、
`pseudo-lowering`、`subtarget` 等尚未配置成 `Backend` 的后端。它们不会出现
在投影层，但会出现在读取层。这正是“契约级视图”和“grep 级视图”的差别。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
T="$W/llvm/utils/TableGen/dsl-surface.py"
DOC="$W/llvm/lib/Target/RVMin/docs/33-field-effect.md"

# 1. 查询一个字段
python3 "$T" --effect TSFlags

# 2. 文档块校验
python3 "$T" --effect TSFlags --check "$DOC"

# 3. 行号漂移后重新生成
python3 "$T" --effect TSFlags --update "$DOC"

# 4. 另外四个对照字段
for f in AltNames Namespace Body Num; do
  python3 "$T" --effect "$f" | head -20
done

# 5. 前四章回归
python3 "$T" --check "$W/llvm/lib/Target/RVMin/docs/29-register-info-dsl.md" \
  --usage "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td" \
          "$W/llvm/lib/Target/RVMin/RVMinCallingConv.td"
python3 "$T" --backend instr-info --check \
  "$W/llvm/lib/Target/RVMin/docs/30-instr-info-dsl.md" \
  --usage "$W/llvm/lib/Target/RVMin/RVMin.td"
python3 "$T" --backend asm-writer --check \
  "$W/llvm/lib/Target/RVMin/docs/31-asm-writer-dsl.md" \
  --usage "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td"
python3 "$T" --status-matrix --check \
  "$W/llvm/lib/Target/RVMin/docs/32-cross-backend-status.md" \
  --usage "$W/llvm/lib/Target/RVMin/RVMinRegisterInfo.td" \
          "$W/llvm/lib/Target/RVMin/RVMin.td"

# 6. 全量门禁
"$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"
```

## 预期输出

```text
$ python3 dsl-surface.py --effect TSFlags --check doc33.md
dsl-surface: OK (field effect: TSFlags, 3 backends)

$ python3 dsl-surface.py --effect TSFlags
影响面：`TSFlags`
...
共 2 个声明类 · 3/3 个后端消费 · 2 个 reader。

$ scripts/rvmin-verify-all.sh "$B/bin"
...
ALL GATES PASS (run rvmin-run.sh separately for the QEMU check)
```

## 常见坑

1. **`--effect` 是字段名，不是 `.td` 行号。** 它查询 schema 字段，不查询某个
   `let` 的位置；行视图请用 `--status-matrix`。
2. **“出现于后端”不等于“被消费”。** 字段可能在该后端 schema 里声明，但
   emitter 不读（如 register-info 的 `AltNames`）。
3. **同名多声明会合并语义。** `TSFlags`、`Size`、`Namespace` 的注解按字段名
   合并；读取点没有静态类型信息，不能自动区分 `Instruction.Size` 与
   `RegisterClass.Size`。
4. **reader 可能比 backend 多。** `--effect` 的读取层包含所有 `-gen-*` 和
   helper；投影层只包含 `BACKENDS` 里配置的后端。
5. **自定义字段查不到是正常结果。** `Num` 的 0/0/0 输出是在提醒你：它不是
   通用 schema 的一部分。
6. **`--effect` 与 `--status-matrix` 互斥。** 两者都只更新自己的生成块，
   不要在一个文档里混用同一个命令。
7. **影响面报告不替代调用图。** 它回答“谁按名字读了”，不回答“值经过了哪些
   变换”；后者要靠派生改写表和 C++ 阅读。

## 练习

1. 用 `--effect Size` 看三个声明类和三个后端的状态，解释为什么它是
   `derive+project`。
2. 用 `--effect AltOrderSelect` 看 `code` 字段的影响面，和 `Body` 对比。
3. 给 `--effect` 增加 `--json` 输出，方便其它脚本消费。
4. 把 `--effect` 与 `--status-matrix` 组合成一个“字段 × 行”的双向查询：
   输入字段名时列出所有使用它的 `.td` 行，需要哪些数据？
5. 为 `asm-matcher` 增加 Backend 后，重新看 `AltNames` / `Namespace` 的
   投影层，哪一列会从 `—` 变成 `project`？

## 产物清单

| 文件 | 变更 |
| --- | --- |
| `llvm/utils/TableGen/dsl-surface.py` | 新增 `--effect FIELD`；声明/投影/reader 三层报告；生成块支持 |
| `llvm/lib/Target/RVMin/docs/33-field-effect.md` | 新增：本章（一个生成块） |
| `llvm/lib/Target/RVMin/docs/README.md` | 新增第 33 章索引行 |
| `llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh` | 第 7 项增加 `--effect TSFlags` 校验 |
