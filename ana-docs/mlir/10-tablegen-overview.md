# 10 `.td` 依赖图总览：ODS 是方言的单一事实来源

前面的章节分析“IR 如何被解析”。从这一章开始换一个方向：IR 是怎么被**声明**出来的。
MLIR 的每个方言、每个 op、每个 attr/type、每条 canonicalization pattern，都先写成
TableGen 记录（`.td`），再由 `mlir-tblgen` 生成 `.inc`，最后被 C++ 包含。

于是“改一个约束会波及多少方言”这类问题，答案不在 C++ 里，而在 `.td` 的 `include` 图上。
本章给出这张图的统计口径、整体形状和关键结论；具体图形见
[11-tablegen-dependency-graph.md](11-tablegen-dependency-graph.md)，单方言解剖见
[12-tablegen-dialect-anatomy.md](12-tablegen-dialect-anatomy.md)。

- 分析基线：分支 `research/mlir`，HEAD `53a80edfeb2e`（基于 `5f7f36636abe`，2026-11-27）。
- 数据由 `mlir/include`、`mlir/lib`、`mlir/python` 三个根目录下的 `.td` 文件全量扫描得到，
  不含 `mlir/test`、`mlir/examples`、`mlir/unittests`。

## 1. 为什么 `.td` 的 include 关系值得单独画一张图

ODS（Operation Definition Specification）把“op 的语义契约”集中到了声明里：

- op 的名字、操作数/结果、trait、接口、verifier、assembly format；
- attr/type 的约束（哪个 `TypeConstraint` 允许哪些类型）；
- pattern（`-gen-rewriters`）与 pass 的选项。

这些声明之间是通过 `include` 复用的：`ArithOps.td` 要写
`include "mlir/Interfaces/SideEffectInterfaces.td"` 才能用 `Pure`；想用
`I32EnumAttr` 就要 `include "mlir/IR/EnumAttr.td"`。因此：

- **include 图 = 声明复用图**。它决定了“某个约束定义在哪里、谁能看见它”。
- 它**不是**编译依赖图。`.td` 不参与 C++ 编译，只有生成的 `.inc` 才被 `#include`。
- 它**也不是** CMake 目标图。`mlir_tablegen()` 把 `.td` 映射成 `.inc`，
  这两张图的节点类型不同（见 §5）。

`mlir/docs/DefiningDialects/Operations.md:85` 说明了这个入口：
“These constructs are defined in `OpBase.td`”。实测 **320 / 412（77.7%）** 的 `.td`
最终依赖 `mlir/IR/OpBase.td`；剩下 92 个走窄路径（`Passes.td` 只要 `PassBase.td`，
`*Enums.td`/`*Dialect.td` 只要 `DialectBase.td`，见 11 章 §6.2）。

## 2. 扫描口径与统计结果

### 2.1 口径

| 项 | 取值 |
| --- | --- |
| 扫描根 | `mlir/include`、`mlir/lib`、`mlir/python`（TableGen 的 `-I` 路径集合） |
| 排除 | `mlir/test`(97)、`mlir/examples`(24)、`mlir/unittests`(2) = 123 个：测试/示例输入，不参与构建 |
| 节点 | 412 个 `.td` |
| 边 | 1132 条 `include "..."` |
| 解析规则 | 依次尝试三个根前缀 + 引用文件自身目录 |

复现方式（脚本随本章提交，输出即本章数字）：

```bash
python3 ana-docs/mlir/tools/td-dependency-graph.py --root .            # 统计
python3 ana-docs/mlir/tools/td-dependency-graph.py --root . \
    --mermaid /tmp/mlir-td.mmd --json /tmp/mlir-td.json                # 全图
```

`llvm/include` 下另有 55 个 `.td`（`Intrinsics*.td`、`Target.td` 等）。它们**不被任何 MLIR `.td`
include**（实测 MLIR 侧 0 条 `include "llvm/..."`），自成一张 LLVM 内部图；MLIR 与它们的
耦合发生在 **CMake 层**，见 §6。

### 2.2 全局数字

```
节点 412   边 1132   环 0
无 include 的叶子   8
无被依赖者的根     148
最大 include 深度   16
```

| 指标 | 值 | 含义 |
| --- | --- | --- |
| 环数 | **0** | include 图是严格 DAG，可以按拓扑序理解 |
| 无 include 的叶子 | 8 | `IR/Utils.td`、`IR/BytecodeBase.td`、`Pass/PassBase.td`、`OpenACC/OpenACCCGEnums.td`、`Bindings/Python/PythonBindings.td`、`SPIRV/{GroupOps,IntelExtOps,NonUniformOps}.td` |
| 深度 0 的节点 | 8 | 与上表相同：深度按“到最深依赖的链长”计算 |
| 根（入度 0） | 148 | 60 python 绑定、38 `Passes.td`、23 方言杂项、6 `TransformOps`、5 `Conversion` 等 |

“根”多并不意味着图松散：148 个根里 98 个是**顶层消费者**（python 绑定 + `Passes.td`），
它们只读不写，天然没有入边。

### 2.3 深度分布

```
depth :  0   1   2   3  4  5   6   7   8   9  10  11  12 13 14 15 16
count :  8  38  12   1  1  5  23  87  43  94  56  22  13  5  2  1  1
```

两个主峰：

- **d=7（87）**：48 个方言的“薄”文件（`*Base.td`/`*Dialect.td`/`*Enums.td`）+ 24 个
  `Interfaces/*.td` + 9 个 `mlir/IR`。它们的共同点是**停在 `OpBase.td`(d=6) 之上**，
  不碰任何 op 生成路径。
- **d=9（94）**：72 个方言的 `*Ops.td` + 17 个 `TransformOps/*.td`。
  这正是“`OpBase` → `Interfaces/*` → 本方言 base”的标准三跳。

下一档 d=10（56）的 39 个是 `python/mlir/dialects/*.td`：绑定层统一比 C++ 侧再深一跳。

d≥13 的 9 个文件全部落在**跨方言复用**上：AMDGPU/ROCm 链（`AMDGPU*`、`ROCDL*`）
与引用最深方言的 python 绑定（`AMDGPUOps.td` 深度 16、`ROCDLOps.td` 14、
`NVVMOps.td`/`TosaOps.td` 13），见 11 章的最长链。

## 3. 六层结构

按“谁为谁提供词汇”而不是按目录，可以把 412 个节点分成六层：

| 层 | 角色 | 代表文件 | 文件数 | 关键性质 |
| --- | --- | --- | --- | --- |
| T0 | 基础谓词 | `mlir/IR/Utils.td` | 1 | 只定义 `CPred`/`And`/`Or` 等组合子，无 include |
| T1 | 约束与方言骨架 | `Constraints.td`、`CommonTypeConstraints.td`、`CommonAttrConstraints.td`、`DialectBase.td`、`Traits.td`、`Properties.td` | 6 | `TypeConstraint`/`AttrConstraint`/`Dialect`/`Trait`/`Property` 基类 |
| T2 | op/attr 骨架 | `AttrTypeBase.td`、`Interfaces.td`、`OpBase.td`、`EnumAttr.td`、`OpAsmInterface.td`、`PatternBase.td`、`BytecodeBase.td` 等 | 11 | `Op`/`AttrDef`/`TypeDef`/`OpInterface` 的最终落点 |
| T3 | Builtin 方言 | `BuiltinDialect.td`、`BuiltinOps.td`、`BuiltinTypes.td`、`BuiltinAttributes.td`、`BuiltinLocationAttributes.td` 等 | 9 | 唯一“人人可见”的方言 |
| T4 | 通用接口 | `mlir/Interfaces/*.td` | 27 | 跨方言契约（side effect、infer type、control flow…） |
| T5 | 具体方言 | `mlir/include/mlir/Dialect/<X>/**/*.td` | 277 | 48 个 `def ... : Dialect` |
| T6 | 方言扩展、pattern 与绑定 | `*/TransformOps/*.td`、`Transform/*Extension/*.td`、`*/Transforms/Passes.td`、`mlir/lib/**/*Canonicalization.td`、`mlir/python/**/*.td`、`mlir/Target` | 81 | 只消费 T5，不被 T5 依赖 |

合计 1 + 6 + 11 + 9 + 27 + 277 + 81 = 412。

注：层号是**概念分层**，不是 depth。例如 `Ext/Linalg`（TransformOps）概念上在 T6，
但 depth 只有 2，因为它只依赖 Linalg 与 Transform 两个方言。

### 3.1 T2 的内部阶梯

整张图最关键的 10 个文件构成一条主路径，外加三条旁支：

```
Utils(0) → Constraints(1) → CommonTypeConstraints(2) → CommonAttrConstraints(3)
        → AttrTypeBase(4) → Interfaces(5) → OpBase(6)          ← 主路径

DialectBase(1) → Utils                  （被 CommonTypeConstraints / AttrTypeBase / OpBase 复用）
Traits(2)      → Constraints            （被 AttrTypeBase / Interfaces / OpBase 复用）
Properties(2)  → Constraints, Utils     （被 EnumAttr / OpBase 复用）
```

主路径决定 depth：`OpBase` = 6 就来自这条链。

`OpBase.td` 位于链顶（depth 6），一次 `include` 就把全部 10 个文件拉进来——
这就是为什么 **127 个文件直接 include `OpBase.td`**：它们实际想要的是“ODS 全套词汇”。

## 4. `include` 的语义与 DAG 约束

### 4.1 词法插入，不是模块导入

`llvm/docs/TableGen/ProgRef.md:242` 写得很直接：

> The content of the included file lexically replaces the `include` directive and is
> then parsed as if it were originally in the main file.

因此 include 图有三个后果：

1. **顺序有意义**：基类必须先于使用它的 `def`/`class` 出现；
2. **没有命名空间**：所有记录共享一个全局命名空间，名字冲突就是硬错误；
3. **必须无环**：A include B、B include A 会形成无限展开。

实测环数为 0，说明这条不变量在 412 个文件上成立。

### 4.2 include guard 是常态，但不是靠它保证无环

TableGen 支持预处理器指令（`ProgRef.md:251`：`#define`/`#ifdef`/`#ifndef`），
所以 MLIR 的 `.td` 普遍写了 C 风格 guard：

```tablegen
#ifndef ARITH_BASE          // mlir/include/mlir/Dialect/Arith/IR/ArithBase.td:9
#define ARITH_BASE
...
#endif // ARITH_BASE
```

统计：**408 / 412 有 `#ifndef` guard**，只有 4 个没有
（`Linalg/TransformOps/LinalgTransformEnums.td`、`Transform/Interfaces/MatchInterfaces.td`、
`lib/Dialect/SPIRV/IR/SPIRVCanonicalization.td`、`lib/Dialect/Shape/IR/ShapeCanonicalization.td`）。

guard 让“同一文件被两条路径引入”不报错，但它**不能**化解真正的循环：
循环展开时 guard 只会让内层提前返回，语义上仍然需要一个拓扑序。
所以设计上仍应把 include 图当 DAG 维护；`mlir-tblgen` 对无 guard 的重复定义会直接报错：

```
$ cat dup.td
include "x.td"
include "x.td"          # x.td: class C { ... }
$ llvm-tblgen -print-records dup.td
x.td:1:7: error: Class 'C' already defined
```

### 4.3 一条实践规则

> 只 include 你**直接使用**的定义，不要为了拿一个类型而 include 整个方言的 op 文件。

反例在 12 章会展开：`ArithCanonicalization.td` 需要 op 定义（pattern 要匹配 op），
所以它 include `ArithOps.td`；但 `ArithOpsInterfaces.td` 只需要接口基类，就只 include
`mlir/IR/OpBase.td`。这条区分让 `lib/**` 的 pattern 文件不会污染方言核心的 include 图。

## 5. 三张图，别混

| 图 | 节点 | 边 | 谁消费 | 用于回答 |
| --- | --- | --- | --- | --- |
| **TableGen include 图**（本章） | `.td` | `include "..."` | `mlir-tblgen` | 声明复用、改动波及面 |
| **CMake 生成图** | `.td` + `.inc` | `mlir_tablegen()` / `LLVM_TARGET_DEPENDS` | ninja | 构建顺序、增量编译 |
| **C++ 编译图** | `.h`/`.inc`/`.cpp` | `#include` | clang | 链接依赖、模块边界 |

`mlir_tablegen()` 定义在 `mlir/cmake/modules/AddMLIR.cmake:5`，它只做两件事：
记录输出文件、把 `${LLVM_TARGET_DEFINITIONS}` 喂给 `mlir-tblgen`。
`add_mlir_dialect()`（同文件 `:145`）是一次生成 6 个 `.inc` 的语法糖：

```cmake
function(add_mlir_dialect dialect dialect_namespace)
  set(LLVM_TARGET_DEFINITIONS ${dialect}.td)
  mlir_tablegen(${dialect}.h.inc -gen-op-decls)
  mlir_tablegen(${dialect}.cpp.inc -gen-op-defs)
  mlir_tablegen(${dialect}Types.h.inc -gen-typedef-decls -typedefs-dialect=${dialect_namespace})
  mlir_tablegen(${dialect}Types.cpp.inc -gen-typedef-defs -typedefs-dialect=${dialect_namespace})
  mlir_tablegen(${dialect}Dialect.h.inc -gen-dialect-decls -dialect=${dialect_namespace})
  mlir_tablegen(${dialect}Dialect.cpp.inc -gen-dialect-defs -dialect=${dialect_namespace})
  add_mlir_dialect_tablegen_target(MLIR${dialect}IncGen)
endfunction()
```

规模：`mlir/include` + `mlir/lib` 下共 **509 处 `mlir_tablegen()`**，最常用的后端是
`-gen-op-decls`/`-gen-op-defs`（各 42）、`-gen-pass-decls`（38）、`-gen-attrdef-*`（33）、
`-gen-enum-*`（34）。这 42 次是**显式**调用；另有 38 次 `add_mlir_dialect()` 在配置期
各展开一次 `-gen-op-decls`/`-gen-op-defs`，所以实际生成 op 声明的次数约 80。

## 6. 构建期生成节点：图上的三个“虚线节点”

有 3 个 `.td` 在源码树里不存在，只在 build 目录出现，但被正常文件 include：

| 生成的 `.td` | 生成方式 | 被谁 include |
| --- | --- | --- |
| `mlir/Dialect/OpenMP/OmpCommon.td` | `mlir_tablegen(OmpCommon.td --gen-directive-decl --directives-dialect=OpenMP)` | `OpenMPEnums.td` |
| `mlir/Dialect/OpenACC/AccCommon.td` | 同上，`--directives-dialect=OpenACC` | `OpenACCOps.td` |
| `mlir/Dialect/Linalg/IR/LinalgNamedStructuredOps.yamlgen.td` | `add_linalg_ods_yaml_gen()`，从 `.yaml` 生成 | `LinalgStructuredOps.td:1425` |

前两个还带出一条**跨项目**依赖（`mlir/include/mlir/Dialect/OpenMP/CMakeLists.txt:3`）：

```cmake
set(LLVM_TARGET_DEFINITIONS ${LLVM_MAIN_INCLUDE_DIR}/llvm/Frontend/OpenMP/OMP.td)
mlir_tablegen(OmpCommon.td --gen-directive-decl --directives-dialect=OpenMP)
```

也就是说 OpenMP/OpenACC 方言的 clause/enum 定义以 **LLVM 的 `OMP.td`/`ACC.td` 为源**，
但这条边只存在于 CMake，`include` 图上完全看不到（MLIR 侧没有任何
`include "llvm/..."`）。这是“只看 `.td` 图会漏判”的唯一一类情况，读图时必须记住。

生成结果可以验证（`builds/pr-ctpop` 下）：

```
tools/mlir/include/mlir/Dialect/OpenMP/OmpCommon.td                    98 行
tools/mlir/include/mlir/Dialect/OpenACC/AccCommon.td                   10 行
tools/mlir/include/mlir/Dialect/Linalg/IR/LinalgNamedStructuredOps.yamlgen.td  5177 行
```

## 7. 关键结论

1. **`mlir/IR` 的 10 个文件是整张图的脊柱**。`OpBase.td` 被 127 个文件直接 include、
   320 个文件传递依赖（占 412 的 77.7%）；`Utils.td` 的传递依赖者达 366（88.8%）。
   改 `OpBase.td` 的类签名 ≈ 改整个 MLIR 的 ODS 方言。
2. **`mlir/Interfaces` 是最大汇聚点，而不是最底层**。`SideEffectInterfaces.td` 直接入度 92
   （仅次于 `OpBase`），因为 `Pure`/`MemoryEffects` 是几乎所有 op 的默认 trait。
   但接口层内部很浅：27 个文件里 24 个只 include `OpBase.td`。
3. **方言之间几乎是平坦的**。48 个方言只有 21 条“方言→方言”核心依赖边，
   最长链 4 跳（`AMDGPU → LLVMIR → GPU → DLTI`）。方言的复杂度主要来自
   对 `mlir/Interfaces` 的横向使用，而不是方言间的纵向堆叠。
4. **`Passes.td` 与 op 定义解耦**。38 个 `Passes.td` 里 32 个只 include `PassBase.td`，
   depth=1，不进方言子图。pass 选项改了不会牵动 op 生成的 `.inc`。
5. **`TransformOps` 是一层独立扩展**。17 个方言的 `TransformOps/` 目录 + 6 个
   `Transform/*Extension/` 目录全部指向 `TransformDialect.td`/`TransformInterfaces.td`，
   但方言核心不反向依赖它们——
   这就是“方言本身不认识 transform”的结构保证。
6. **python 绑定是纯消费者**。60 个 `python/mlir/dialects/*.td` 全部入度 0，
   每个文件只 include 对应方言的 `.td`，因此加绑定不会影响任何 C++ 生成物。

下一章：[11-tablegen-dependency-graph.md](11-tablegen-dependency-graph.md) 给出完整依赖图。
