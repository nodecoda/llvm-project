# 12 单方言 `.td` 解剖：从 Arith 到 Linalg

前两章看的是全图。本章把镜头拉近到一个方言内部：一个方言到底由几个 `.td` 组成、
每个文件的职责边界在哪、CMake 如何把它们变成 `.inc`。
以 **Arith**（最小完整样本）和 **Linalg**（多文件 + 分片 + YAML 生成）为例。

- 分析基线：`research/mlir` / `53a80edfeb2e`（基于 `5f7f36636abe`）。

## 1. 标准布局

一个典型方言在 `.td` 层面有 4–6 个文件，职责互不重叠：

| 角色 | 典型文件 | 典型 include | 生成目标 |
| --- | --- | --- | --- |
| 方言定义 + 枚举 + 约束 | `<X>Base.td` / `<X>Dialect.td` | `OpBase.td`、`EnumAttr.td` | `-gen-dialect-decls/defs`、`-gen-enum-*`、`-gen-typedef-*` |
| op 定义 | `<X>Ops.td` | `<X>Base.td` + 所需 `mlir/Interfaces/*` | `-gen-op-decls/defs` |
| 方言自己的 op 接口 | `<X>OpsInterfaces.td` | 通常只有 `OpBase.td` | `-gen-op-interface-decls/defs` |
| 属性 | `<X>Attributes.td`（可选） | `AttrTypeBase.td` 系 | `-gen-attrdef-decls/defs` |
| pass 声明 | `Transforms/Passes.td` | **只有** `PassBase.td` | `-gen-pass-decls/defs` |
| 规范化 pattern | `lib/Dialect/<X>/IR/<X>Canonicalization.td` | `PatternBase.td` + `<X>Ops.td` | `-gen-rewriters` |

三条边界规则：

1. **`Base` 不 include `Ops`**，`Ops` include `Base`。这样 `Base` 可以被别的方言单独复用
   （例如 `ArithBase.td` 被 `Affine`/`Complex`/`MemRef`/`Vector` 引用）。
2. **`Passes.td` 不 include `Ops.td`**。38 个 `Passes.td` 里 32 个只 include `PassBase.td`，
   保证 pass 选项变更不触发 op 重新生成。
3. **pattern 文件属于 `lib/`，可以 include `Ops.td`**。它需要看到 op 定义才能写
   `Pat<(Arith_AddIOp ...)>`，但它是叶子，不会被任何 `.td` 反向引用。

## 2. Arith：最小完整样本

### 2.1 文件与职责

```
mlir/include/mlir/Dialect/Arith/
├── IR/
│   ├── ArithBase.td           dialect 定义 + 5 组枚举 + attr 包装
│   ├── ArithOps.td            47 个 op
│   ├── ArithOpsInterfaces.td  4 个 op interface
│   └── CMakeLists.txt
└── Transforms/Passes.td       6 个 pass
mlir/lib/Dialect/Arith/IR/
├── ArithCanonicalization.td   46 条 Pat<> + 43 个辅助 def
└── CMakeLists.txt
```

`ArithBase.td`（194 行）的内容分布：

- `Arith_Dialect`（`:15`）：`name = "arith"`、`cppNamespace = "::mlir::arith"`、
  `hasConstantMaterializer = 1`、`hasAttributes = 1`；
- 枚举：`Arith_CmpFPredicateAttr`（`:49`）、`Arith_CmpIPredicateAttr`（`:72`）、
  `AtomicRMWKindAttr`（`:106`）、`FastMathFlags`（`:132`）、`Arith_RoundingModeAttr`（`:187`）；
- attr 包装：`Arith_FastMathAttr`（`:145`）、`Arith_IntegerOverflowAttr`（`:166`）、
  `Arith_RoundingModeAttr`（`:187`）。

为什么枚举放在 `Base` 而不是 `Ops`？因为 `Complex`、`Math` 等方言要复用
`FastMathFlags`/`RoundingMode`。把枚举留在 `ArithOps.td` 会迫使它们 include 47 个 arith op。

### 2.2 真实的 include 列表

`ArithOps.td:12-23` 一共 12 条 include，可以按来源分三类：

```tablegen
include "mlir/Dialect/Arith/IR/ArithBase.td"        // 本方言
include "mlir/Dialect/Arith/IR/ArithOpsInterfaces.td"

include "mlir/Interfaces/CastInterfaces.td"          // 跨方言契约
include "mlir/Interfaces/ControlFlowInterfaces.td"
include "mlir/Interfaces/InferIntDivisibilityOpInterface.td"
include "mlir/Interfaces/InferIntRangeInterface.td"
include "mlir/Interfaces/InferTypeOpInterface.td"
include "mlir/Interfaces/SideEffectInterfaces.td"
include "mlir/Interfaces/VectorInterfaces.td"

include "mlir/IR/BuiltinAttributeInterfaces.td"      // IR 内核
include "mlir/IR/OpAsmInterface.td"
include "mlir/IR/EnumAttr.td"
```

注意没有一条是 `include "mlir/IR/OpBase.td"`：`ArithBase.td` 已经 include 了它，
`ArithOps.td` 通过 `ArithBase.td` 间接获得。这是 §1 的规则 1 在起作用。

### 2.3 CMake：一次 `add_mlir_dialect` 生成 6 个 `.inc`

`mlir/include/mlir/Dialect/Arith/IR/CMakeLists.txt` 全文只有 10 行，却等价于
**12 个 `.inc`**：4 次显式 `mlir_tablegen` + `add_mlir_dialect` 的 6 次 + `add_mlir_interface` 的 2 次。

```cmake
set(LLVM_TARGET_DEFINITIONS ArithOps.td)
mlir_tablegen(ArithOpsEnums.h.inc -gen-enum-decls)            # 枚举来自 ArithBase.td
mlir_tablegen(ArithOpsEnums.cpp.inc -gen-enum-defs)
mlir_tablegen(ArithOpsAttributes.h.inc -gen-attrdef-decls -attrdefs-dialect=arith)
mlir_tablegen(ArithOpsAttributes.cpp.inc -gen-attrdef-defs -attrdefs-dialect=arith)
add_mlir_dialect(ArithOps arith)                              # 再展开 6 次
add_mlir_doc(ArithOps ArithOps Dialects/ -gen-dialect-doc)
add_mlir_interface(ArithOpsInterfaces)                        # -gen-op-interface-*
```

`add_mlir_dialect(ArithOps arith)`（`AddMLIR.cmake:145`）展开为：

| 输出 | 后端 | 内容 |
| --- | --- | --- |
| `ArithOps.h.inc` | `-gen-op-decls` | 47 个 op 的 C++ 类声明 |
| `ArithOps.cpp.inc` | `-gen-op-defs` | 构造/解析/打印/verify 实现 |
| `ArithOpsTypes.h.inc` / `.cpp.inc` | `-gen-typedef-*` | arith 自定义类型（当前为空但接口保留） |
| `ArithOpsDialect.h.inc` / `.cpp.inc` | `-gen-dialect-*` | `ArithDialect` 类与注册 |

`add_mlir_interface(ArithOpsInterfaces)` 输出 `-gen-op-interface-decls/defs`，
把 `ArithFastMathInterface` 等 4 个接口变成 `OpInterface` 的 CRTP 基类。

消费者的位置是固定的：`ArithOps.h` include `ArithOps.h.inc`，
`ArithOps.cpp` include `ArithOps.cpp.inc`；`ArithDialect.cpp` include
`ArithOpsDialect.cpp.inc` 并调用 `addOperations<...>()`。

### 2.4 谁依赖 Arith

在方言级图上，Arith 的入度是 6（`Affine`/`Complex`/`Math`/`MemRef`/`Vector`/`XeGPU`），
全部通过 `ArithBase.td` 而非 `ArithOps.td`。这就是把 `Base` 拆出来的直接收益：
**6 个方言只为拿枚举，却不用背 47 个 op 的生成**。

## 3. Linalg：多文件、分片与 YAML 生成

Linalg 的 op 分成三个 partition，并且有一半来自 YAML。

### 3.1 文件布局

```
mlir/include/mlir/Dialect/Linalg/IR/
├── LinalgBase.td              dialect + 枚举 + 约束（include StructuredOpsUtils.td）
├── LinalgInterfaces.td        方言自己的 op interface
├── LinalgOps.td               6 个“核心” op          partition = Core
├── LinalgStructuredOps.td     11 个结构化 op          partition = Structured
│   └── :1425 include "…/LinalgNamedStructuredOps.yamlgen.td"
├── LinalgRelayoutOps.td       2 个 relayout op        partition = Relayout
├── LinalgAllOps.td            仅 3 条 include，把上面三个聚合
├── LinalgEnums.td             枚举
├── RelayoutOpInterface.td
└── LinalgDoc.td               只用于文档生成
```

`LinalgAllOps.td` 是一个“聚合入口”，内容只有：

```tablegen
include "mlir/Dialect/Linalg/IR/LinalgOps.td"
include "mlir/Dialect/Linalg/IR/LinalgStructuredOps.td"
include "mlir/Dialect/Linalg/IR/LinalgRelayoutOps.td"
```

它存在的原因是：`-gen-op-decls -op-partition=Core` 需要一次看到**所有** op，
才能校验“每个 op 都有 partition、且 partition 名唯一”
（`mlir/tools/mlir-tblgen/PartitionHelpers.h:38`）。

### 3.2 partition 字段

partition 是 op 的一个普通字段（`mlir/include/mlir/IR/OpBase.td:364-368`）：

```tablegen
// LinalgOps.td:29
let partition = Linalg_CorePartition;
```

`Linalg_CorePartition` 是 `DialectBase.td:140` 的 `class Partition<string name>` 实例。
生成时用 `-op-partition=Core` 过滤（`Linalg/IR/CMakeLists.txt:66-68`）：

```cmake
set(LLVM_TARGET_DEFINITIONS LinalgAllOps.td)
mlir_tablegen(LinalgOps.h.inc -gen-op-decls -op-partition=Core)
mlir_tablegen(LinalgOps.cpp.inc -gen-op-defs -op-partition=Core)
```

三个 partition 各生成一对 `.inc`，好处是**并行与增量**：改一个 structured op
不会重编 Core 的 6 个 op 声明。

### 3.3 YAML 生成链

`LinalgNamedStructuredOps.yaml`（59 个 `!LinalgOpConfig`）不是手写 `.td`，
而是由 `add_linalg_ods_yaml_gen()`（`Linalg/IR/CMakeLists.txt:2-33`）在构建期转换：

```
LinalgNamedStructuredOps.yaml
  → mlir-linalg-ods-yaml-gen -o-ods-decl=…/LinalgNamedStructuredOps.yamlgen.td
                             -o-impl=…/LinalgNamedStructuredOps.yamlgen.cpp.inc
  → LinalgStructuredOps.td:1425 include 生成的 .td
  → mlir-tblgen 再按普通 ODS 处理
```

生成物规模：`.yamlgen.td` 5177 行、59 个 `def …Op`。
这解释了为什么 `LinalgStructuredOps.td` 只有 11 个手写 op，
但 Linalg 的实际 op 数量远多于此——**YAML 是二等公民，最终仍变成 ODS**。

这段 CMake 还留了一条关于依赖顺序的注释（`Linalg/IR/CMakeLists.txt:28-43`）：
`add_mlir_interface` 必须在 `LLVM_TARGET_DEPENDS` 被更新**之前**调用，
否则 `mlir-headers → mlir-generic-headers` 与新的 tablegen 目标会形成 CMake 循环依赖。
这是“CMake 生成图”与“`.td` include 图”不同的直接证据。

## 4. TransformOps 扩展：方言如何“外挂”到 transform

`Dialect/<X>/TransformOps/*.td` 是给 transform 方言注册的 op，结构高度一致：

```tablegen
// Linalg/TransformOps/LinalgTransformOps.td:12-22
include "mlir/Dialect/Linalg/IR/LinalgEnums.td"
include "mlir/Dialect/Transform/IR/TransformAttrs.td"
include "mlir/Dialect/Transform/IR/TransformDialect.td"
include "mlir/Dialect/Transform/Interfaces/TransformInterfaces.td"
include "mlir/Dialect/Transform/IR/TransformTypes.td"
include "mlir/Dialect/SCF/IR/DeviceMappingInterface.td"
include "mlir/Interfaces/SideEffectInterfaces.td"
include "mlir/Interfaces/InferTypeOpInterface.td"
include "mlir/IR/OpBase.td"
include "mlir/IR/RegionKindInterface.td"
```

关键约束：**这些文件只能被 transform 的生成目标消费**。
`LinalgOps.td` 不 include 它们，所以 `Linalg` 方言本身不依赖 `transform`。
实现方式是在 CMake 里给 `TransformOps` 单独的 tablegen target，而不是在
`Linalg/IR/CMakeLists.txt` 里生成（对比 `Linalg/TransformOps/CMakeLists.txt`）。

## 5. `Passes.td`：与 op 解耦的声明

`mlir/include/mlir/Dialect/Arith/Transforms/Passes.td` 只有一条 include：

```tablegen
include "mlir/Pass/PassBase.td"
```

生成 4 类产物（`-gen-pass-decls`/`-gen-pass-defs` + 两组 C API）。
因为不 include `ArithOps.td`，pass 的 option 结构与 op 定义完全独立：
改 `--arith-emulate-wide-int` 的默认值不会触发 `ArithOps.cpp.inc` 重新生成。

反例是 `OpPassManager` 里需要 op 名的 pass（如 `-test-…`），
它们在 `.cpp` 里 `#include` op 头文件即可，不需要在 `.td` 里建立依赖。

## 6. Canonicalization pattern：唯一合法“反向 include”

`mlir/lib/Dialect/Arith/IR/ArithCanonicalization.td`：

```tablegen
include "mlir/IR/PatternBase.td"
include "mlir/Dialect/Arith/IR/ArithOps.td"
```

46 条 `Pat<>` 需要匹配具体 op，所以必须看到 `ArithOps.td`。
它在 include 图上是一个**叶子**（入度 0），因此不会把 pattern 的依赖
反向传染给方言核心：

```cmake
set(LLVM_TARGET_DEFINITIONS ArithCanonicalization.td)
mlir_tablegen(ArithCanonicalization.inc -gen-rewriters)   # lib/Dialect/Arith/IR/CMakeLists.txt:9-10
add_public_tablegen_target(MLIRArithCanonicalizationIncGen)
```

`-gen-rewriters` 生成的是 `RewritePattern` 的 C++ 代码，直接编译进
`MLIRArithDialect` 库，不产生任何 `.h.inc` 给外部使用。

## 7. 新增一个方言的 checklist

1. `include/mlir/Dialect/<X>/IR/<X>Base.td`：只放 `Dialect` def、枚举、约束；
   include `OpBase.td`（必要时加 `EnumAttr.td`）。**不要** include `Ops.td`。
2. `include/mlir/Dialect/<X>/IR/<X>Ops.td`：include `Base.td` + 你实际用到的
   `mlir/Interfaces/*.td`。不要为了“以后可能用”而多 include。
3. 有自定义 op interface 时单独放 `<X>OpsInterfaces.td`，只 include `OpBase.td`。
4. `Transforms/Passes.td` 只 include `PassBase.td`。
5. CMake：`add_mlir_dialect(<X>Ops <x>)` + `add_mlir_interface(<X>OpsInterfaces)`；
   pattern 用 `-gen-rewriters` 单独建 target。
6. 给所有文件加 `#ifndef` guard（仓库 408/412 都这么做），名字用大写路径。
7. 若 op 数量大（>100），考虑 `partition` 或 `-op-shard-count` 分片（`AddMLIR.cmake:157`）。
8. 若 op 由外部 DSL 生成，走 Linalg 的模式：先产出 `.yamlgen.td`，
   再让手写 `.td` include 它，并把生成物标 `GENERATED TRUE`。

## 8. 常见错误

| 症状 | 原因 | 修法 |
| --- | --- | --- |
| `Class 'X' already defined` | 无 guard 的 `.td` 被两条路径 include | 加 `#ifndef` guard |
| `Variable not defined: 'Pure'` | 漏 include `SideEffectInterfaces.td` | 补 include，而不是抄一份 `Pure` |
| 方言 A 的构建突然依赖方言 B 的 op | A 的 `Base.td` include 了 B 的 `Ops.td` | 改为只 include B 的 `Base.td`，或把共享定义下沉 |
| `-gen-op-decls` 报 partition 缺失 | 用了 `-op-partition` 但某个 op 没设 `partition` | 给每个 op 加 `let partition = …` |
| 改了 pass option 触发全量重编 | `Passes.td` include 了 `Ops.td` | 删掉这条 include |
| transform 扩展的 op 被普通方言看到 | `Ops.td` include 了 `TransformOps` | 移到独立的 `TransformOps/` 目录与 target |

---

回到总览：[10-tablegen-overview.md](10-tablegen-overview.md)；
图形版：[11-tablegen-dependency-graph.md](11-tablegen-dependency-graph.md)。
