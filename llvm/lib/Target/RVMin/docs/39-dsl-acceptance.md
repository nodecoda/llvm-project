# 第 39 章：实验课验收——七个后端与契约的边界

## 目标

第 29–38 章把同一套 `(declaring class, field)` 契约依次套在七个
`-gen-*` 后端上。本章不再新增后端，而是回答三个收尾问题：

1. 这套方法到底证明了什么？
2. 哪些结论是可复用的判据，哪些只是一次性观察？
3. 方法的边界在哪里——它明确**不能**回答什么？

验收以可执行命令为准，不以叙述为准。九个判据（B-AC1–B-AC9）覆盖：

- 七个后端的契约都能构建、校验；
- 每个被消费字段都有语义注解，且没有过期注解；
- 跨后端矩阵与字段视图都覆盖七个后端；
- 生成块可复现；
- 上游文档与全量门禁保持同步。

非目标：

- 不新增第八个后端；
- 不把契约扩成调用图或 C++ 类型推导；
- 不覆盖 X86 / WebAssembly 的自定义 emitter 路径。

## 背景：为什么实验课也需要验收

上篇、下篇、第三篇分别用第 14、23、28 章做验收；29–38 章也需要同样的
收口。区别在于：前三篇验收的是“后端能跑”，本篇验收的是“契约可信”：

- 能不能从 emitter 反推出稳定的 `(declaring class, field)` 模型？
- 这个模型能不能跨七个后端复用，而不是每个后端一套说辞？
- 它能不能明确说出自己的边界，而不是把未知包装成已知？

因此本章的验收项全部是可执行命令，而不是主观描述。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `llvm/utils/TableGen/dsl-surface.py` | 七个 Backend 配置、导出/校验逻辑、`--summary` |
| `llvm/lib/Target/RVMin/docs/29-38-*.md` | 每个后端的契约、投影矩阵与派生改写 |
| `llvm/lib/Target/RVMin/docs/32-cross-backend-status.md` | 行视图：13 处 `.td` 用法 × 七后端 |
| `llvm/lib/Target/RVMin/docs/33-field-effect.md` | 字段视图：一个字段 × 七后端 |
| `llvm/lib/Target/RVMin/docs/39-dsl-acceptance.md` | 本章：B-AC1–B-AC9 |
| `llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh` | 第 7 项门禁：七个契约 + 矩阵 + 字段 + 总表 |
| `llvm/docs/TableGen/BackEnds.md` | 上游方法论文档与 `--summary` 示例 |

## 七后端总表

下面这段由 `--summary` 生成，不要手改；改后端配置后运行 `--update`。

<!-- BEGIN: GENERATED BACKEND SUMMARY -->

| backend | roots | 字段 | 消费 | derive | project | both | schema | magic | derivations |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `register-info` | 8 | 59 | 44 | 39 | 4 | 1 | 15 | 10 | 11 |
| `instr-info` | 6 | 111 | 85 | 74 | 5 | 6 | 26 | 0 | 8 |
| `asm-writer` | 8 | 130 | 99 | 84 | 9 | 6 | 31 | 2 | 8 |
| `asm-matcher` | 15 | 166 | 142 | 95 | 42 | 5 | 24 | 2 | 7 |
| `dag-isel` | 14 | 186 | 136 | 135 | 1 | 0 | 50 | 6 | 7 |
| `fast-isel` | 14 | 186 | 136 | 133 | 1 | 2 | 50 | 0 | 4 |
| `disassembler` | 8 | 127 | 96 | 88 | 4 | 4 | 31 | 0 | 4 |

共 7 个后端 · 73 个 root 条目 · 965 个后端字段条目 · 738 个被消费 · 20 个 magic name · 49 条派生改写。
<!-- END: GENERATED BACKEND SUMMARY -->

读表注意：

- **965 个后端字段条目**是“每个后端的字段数”之和，不是 965 个唯一字段名；
  同一个 `TSFlags` 会在七个后端里各出现一次。
- **738 个被消费** = 648 `derive` + 66 `project` + 24 `derive+project`；
  剩下 227 个是 `schema`（看得到、本后端不读）。
- **20 个 magic name** 只来自 `register-info`（10）、`asm-writer`（2）、
  `asm-matcher`（2）、`dag-isel`（6）；其余三个后端为 0。
- **49 条派生改写**说明“record → 生成物”之间永远有一层共享 C++。

## 契约模型回顾

七章下来，`record = schema，gen = 投影` 这句话可以展开成五层：

| 层 | 证据 | 契约里的表示 |
| --- | --- | --- |
| 声明式 schema | `Target.td` / `TargetSubtarget.td` | `schema` |
| 共享派生 | `CodeGenInstruction` / `CodeGenRegisters` / `CodeGenDAGPatterns` | `derive` |
| emitter 投影 | 各 `*Emitter.cpp` | `project` / `derive+project` |
| 过程式规则 | fast-isel 的“简单 pattern”过滤 | **不在字段里** |
| target 自定义 | `field bits<32> Inst` | `--usage` 标“自定义 / 不在契约” |

这五层解释了为什么“record 是完整模型”只在**声明式语义**上成立：

- 字段、类型、默认值、约束确实都在 record 里；
- 但“某个后端是否接受这个 pattern”“某个 bit 是否被解码”是过程式的，
  只能从 emitter 反推；
- 反汇编器的核心输入甚至不在通用 schema 里，而是 target 自己声明的。

## 七条可复用判据

1. **stage 是 `(backend, field)` 的函数。**
   `TSFlags` 七个后端都消费；`CodeSize` 在 dag-isel 是 `project`、在
   fast-isel 是 `schema`；`FastIselShouldIgnore` 在 dag-isel 是 `schema`、
   在 fast-isel 是 `project`。
2. **schema 闭包不等于基类闭包。**
   `collect_fields()` 只沿基类展开；`ImmLeaf` 的派生类字段必须通过
   `helper_classes` 显式引入，否则会静默消失。
3. **magic name 是 source set 的属性，不是后端大小的属性。**
   `register-info` 有 10 个，fast-isel / disassembler 都是 0。
4. **一个 `-gen-*` 选项不一定对应一个 emitter。**
   `-gen-disassembler` 是分发器；契约必须声明自己覆盖哪条路径。
5. **后端的主输入可能不在通用 schema 里。**
   `Inst` 是 target-custom；工具必须点名它，而不是假装它不存在。
6. **注解是契约的一部分。**
   `--check` 会同时报 missing annotation 和 stale annotation；加字段却不
   写语义，门禁失败。
7. **文档是漂移检查对象。**
   `--update` 后 `--check` 必须干净；生成块不允许手改。

## 验收标准

| AC | 判据 | 命令 | 期望 |
| --- | --- | --- | --- |
| B-AC1 | 七个后端都可构建 | `dsl-surface.py --summary` | 7 行；73 roots / 965 条目 / 738 消费 |
| B-AC2 | 七个后端文档与 emitter 同步 | 七个 `--backend ... --check` | 全部 `OK` |
| B-AC3 | 无缺失/过期注解 | 同上 | 不打印 missing/stale |
| B-AC4 | 跨后端矩阵覆盖七后端 | `--status-matrix --check` | `13 usages, 7 backends` |
| B-AC5 | 字段视图覆盖七后端 | `--effect TSFlags --check` | `7/7` 后端消费 |
| B-AC6 | 同一字段可投影不同 | `--effect EncodingInfos` | 只有 disassembler `project` |
| B-AC7 | 生成块可复现 | `--update` 后 `git diff --exit-code` | 无 diff |
| B-AC8 | 上游文档列全七后端 | 门禁内的 BackEnds 检查 | `PASS 7/7` |
| B-AC9 | 全量门禁 | `rvmin-verify-all.sh` | `ALL GATES PASS` |

### B-AC1：七后端总表

```bash
python3 llvm/utils/TableGen/dsl-surface.py --summary
```

期望末行：

```text
共 7 个后端 · 73 个 root 条目 · 965 个后端字段条目 · 738 个被消费 · 20 个 magic name · 49 条派生改写。
```

### B-AC2 / B-AC3：逐后端校验

```bash
T=llvm/utils/TableGen/dsl-surface.py
D=llvm/lib/Target/RVMin/docs
export PYTHONDONTWRITEBYTECODE=1

python3 "$T" --check "$D/29-register-info-dsl.md" \
  --usage llvm/lib/Target/RVMin/RVMinRegisterInfo.td \
          llvm/lib/Target/RVMin/RVMinCallingConv.td
python3 "$T" --backend instr-info --check "$D/30-instr-info-dsl.md" \
  --usage llvm/lib/Target/RVMin/RVMin.td
python3 "$T" --backend asm-writer --check "$D/31-asm-writer-dsl.md" \
  --usage llvm/lib/Target/RVMin/RVMinRegisterInfo.td
python3 "$T" --backend asm-matcher --check "$D/34-asm-matcher-dsl.md" \
  --usage llvm/lib/Target/RVMin/RVMin.td \
          llvm/lib/Target/RVMin/RVMinRegisterInfo.td
python3 "$T" --backend dag-isel --check "$D/36-dag-isel-dsl.md" \
  --usage llvm/lib/Target/RVMin/RVMinInstrInfo.td \
          llvm/lib/Target/RVMin/RVMinRegisterInfo.td
python3 "$T" --backend fast-isel --check "$D/37-fast-isel-dsl.md" \
  --usage llvm/lib/Target/RVMin/RVMinInstrInfo.td \
          llvm/lib/Target/RVMin/RVMinRegisterInfo.td
python3 "$T" --backend disassembler --check "$D/38-disassembler-dsl.md" \
  --usage llvm/lib/Target/RVMin/RVMinInstrInfo.td \
          llvm/lib/Target/RVMin/RVMinInstrFormats.td \
          llvm/lib/Target/RVMin/RVMinRegisterInfo.td
```

每条都应打印 `dsl-surface: OK (...)`。如果某个后端新增了消费字段却没写
注解，这里会变成 `FAILED` 并列出字段名。

### B-AC4 / B-AC5：聚合视图

```bash
python3 "$T" --status-matrix --check "$D/32-cross-backend-status.md" \
  --usage llvm/lib/Target/RVMin/RVMinRegisterInfo.td \
          llvm/lib/Target/RVMin/RVMin.td
python3 "$T" --effect TSFlags --check "$D/33-field-effect.md"
```

期望：

```text
dsl-surface: OK (cross-backend status: 13 usages, 7 backends)
dsl-surface: OK (field effect: TSFlags, 7 backends)
```

### B-AC6：同一字段的不同命运

```bash
python3 "$T" --effect EncodingInfos | tail -3
```

期望：

```text
共 1 个声明类 · 1/7 个后端消费 · 3 个 reader。
```

只有 `disassembler` 把它当 `project`；dag-isel / fast-isel 是 `未消费`。
`Inst` 本身则根本不在通用 schema 里，只能在 `--usage` 中作为 target-custom
字段被点名。

### B-AC7：生成块可复现

```bash
python3 "$T" --summary --update "$D/39-dsl-acceptance.md"
python3 "$T" --summary --check "$D/39-dsl-acceptance.md"
python3 "$T" --status-matrix --update "$D/32-cross-backend-status.md" \
  --usage llvm/lib/Target/RVMin/RVMinRegisterInfo.td \
          llvm/lib/Target/RVMin/RVMin.td
python3 "$T" --status-matrix --check "$D/32-cross-backend-status.md" \
  --usage llvm/lib/Target/RVMin/RVMinRegisterInfo.td \
          llvm/lib/Target/RVMin/RVMin.td
git diff --exit-code "$D/32-cross-backend-status.md" "$D/39-dsl-acceptance.md"
```

`--update` 之后再跑 `--check` 必须仍为 OK，且 `git diff` 为空。

### B-AC8 / B-AC9：上游文档与全量门禁

```bash
bash llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh \
  /home/ubuntu/llvm/builds/pr-ctpop/bin
```

期望结尾：

```text
PASS 7/7: TableGen DSL contracts + status matrix + field effect
ALL GATES PASS (run rvmin-run.sh separately for the QEMU check)
```

## 方法边界

这套契约明确**不**做以下事情；把它们写清楚，比假装覆盖更重要：

| 不做的 | 后果 | 需要什么才能做 |
| --- | --- | --- |
| C++ 类型推导 | `TSFlags` / `Size` 的同名多声明会合并语义 | Clang AST / 调用点类型 |
| 调用图 | 只知道“谁按名字读了”，不知道值怎么变换 | 数据流分析 |
| 过程式合法性 | fast-isel 的“简单 pattern”过滤不在字段里 | 读 emitter 的控制流 |
| target 自定义 schema | `Inst` 只能标“自定义/不在契约” | 解析 target 自己的 `.td` |
| X86 / WebAssembly 自定义路径 | `-gen-disassembler` 契约只覆盖 generic path | 按 target 配置 source set |
| 全部 43+ 个 `-gen-*` | 只覆盖七个有代表性的后端 | 逐个补 Backend 配置 |

这也是第 38 章结尾那句话的完整含义：**契约的价值不只在于它描述了什么，
还在于它能诚实说出自己描述不了什么。**

## 常见坑

1. **965 不是唯一字段数。** 它是七个后端的字段条目之和；同一个
   `TSFlags`、`Size`、`Namespace` 会在多个后端里重复计数。
2. **stage 不是字段的固有属性。** `CodeSize` 在 dag-isel 是 `project`、
   在 fast-isel 是 `schema`；`EncodingInfos` 在 disassembler 是
   `project`、在其它后端是 `未消费`。
3. **`--summary` 与 `--summary --check` 不是一回事。** 前者只渲染表格；
   后者会构建七个契约，并检查 missing / stale 注解。
4. **`--update` 之后必须 `--check`。** 手改生成块会在下一次 `--update`
   被覆盖；`git diff` 为空才是可复现的证据。
5. **`--effect Inst` 查不到是正常的。** `Inst` 是 target-custom 字段，
   不在通用 schema 里；它只在 `--usage` 中以“自定义/不在契约”出现。
6. **49 条派生改写不是全部改写。** 它只覆盖已配置的七个后端；新增后端
   必须自己声明 derivation，否则契约会少一层。

## 后续方向

1. **第八个后端**：`-gen-subtarget`（`Predicate` / `SubtargetFeature`）、
   `-gen-callingconv`、`-gen-global-isel` 都是自然候选。
2. **跨 target 验证**：把同一套 `--backend` 配置跑在一个真实 target 上，
   看字段数和 magic name 如何变化。
3. **CI 化**：把 `--summary --check` 与七个 `--check` 放进目标仓库门禁，
   阻止“加字段不写语义”。
4. **契约驱动的迁移**：改一个字段前，用 `--effect` 一次性拿到所有后端与
   读取点，替代 grep。
5. **回到主线 A**：RVMin 后端本身还有优化、labs、更多扩展的空间；契约
   工具可以作为修改前的影响面检查器。

## 练习

1. 给 `--summary` 增加一列 `main_output`，说明每个后端生成哪个 `.inc`。
2. 把七个 `--check` 收成一个 `--check-all` 子命令，并保持错误信息可定位。
3. 选一个字段（如 `Size`），用 `--effect` 解释它为什么在七个后端里出现
   不同的 stage。
4. 给 `--usage` 增加“位段赋值”解析，回答“哪些 bit 由哪个格式类写入”。
5. 预测第八个后端 `-gen-subtarget` 的 roots / project 比例，再用实际配置
   验证。

## 产物清单

| 文件 | 变更 |
| --- | --- |
| `llvm/utils/TableGen/dsl-surface.py` | 新增 `--summary`；docstring 增加聚合视图说明 |
| `llvm/lib/Target/RVMin/docs/39-dsl-acceptance.md` | 新增：实验课验收（B-AC1–B-AC9） |
| `llvm/lib/Target/RVMin/docs/README.md` | 新增第 39 章索引行；范围改为 29–39 |
| `llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh` | 第 7 项增加 `--summary --check` |
| `llvm/docs/TableGen/BackEnds.md` | 增加 `--summary` 示例 |
