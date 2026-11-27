# 09 易错点、内部契约与案例分析

本章汇总阅读/修改 `mlir/lib/AsmParser/` 时最容易踩的坑，并用本工作区实际修复的
一个 printer 缺陷说明“parse/print 对称”契约。

## 1. 词法层易错点

| 现象 | 原因 |
| --- | --- |
| `%0a` 报 `invalid SSA name` | suffix 以数字开头时后续只能全数字（`Lexer.cpp:383-400`） |
| `%x#1` 是两个 token | `#1` 是独立 `hash_identifier`，表示结果号（`Parser.cpp:1118-1128`） |
| `0xi32` 被切成 `0` + `xi32` | `0x` 后无十六进制位只认 `0`（`Lexer.cpp:313-316`） |
| `1e5` 不是浮点 | 浮点必须以 `.` 为前提（`Lexer.cpp:329-331`） |
| `func` 是关键字、`func.func` 不是 | 关键字匹配整段拼写；`.` 属于裸标识符字符 |
| `-1` 是两个 token | 负号由语法层处理，不是数字的一部分 |
| `i0` 也能切成 `inttype` | 词法只检查“全数字”，宽度由类型解析校验 |

## 2. 三种“别名”的前向引用语义不同

| 别名 | 表 | 前向引用 | 依据 |
| --- | --- | --- | --- |
| 属性别名 `#x` | `attributeAliasDefinitions` | **不允许** | `DialectSymbolParser.cpp:200-208` |
| 类型别名 `!x` | `typeAliasDefinitions` | **不允许** | 同上（共用 `parseExtendedSymbol`） |
| location 别名 `#loc` | 属性别名表 | **允许**（`OpaqueLoc` + `finalize` 回填） | `Parser.cpp:2237-2260`、`:944-975` |

写 dialect/工具代码时不要假设“别名总是已定义”；反向也不要假设 location alias
一定先定义。

## 3. 默认方言栈：省略前缀的真实规则

- 初始栈是 `{"builtin"}`（`ParserState.h:93`），所以顶层 `module { ... }` 合法；
- 进入实现了 `OpAsmOpInterface::getDefaultDialect()` 的 op 的 region 时，
  该方言被压栈（`Parser.cpp:2181-2191`）；
- 于是 `omp.parallel { omp.terminator }` 里可以写 `terminator`，
  但**只在 region 内**有效；
- 名字里已经含 `.` 时永远按全名解析，不受默认方言影响。

## 4. `: type` 属于外层语法

`#dialect.attr<...> : i32` 的 `: i32` 在**调用 dialect 之前**解析
（`DialectSymbolParser.cpp:262-267`），语义是“期望的 attribute type”。
dialect 的 `parseAttribute` 看不到这个类型标注；它拿到的 `attrType` 是解析结果
（可能为 null）。

同理，`parseExtendedAttr` 末尾会做一次 `TypedAttr` 类型一致性检查
（`:288-294`），所以 `#foo.a : i32` 里的 attr 若声明自己是 f32 会直接报错。

## 5. 延迟 SSA 的内部约定（不要在扩展代码里依赖）

- 前向引用的占位值是一个 `builtin.unrealized_conversion_cast` 的结果
  （`Parser.cpp:1242-1258`）。这是实现细节，不是语言契约；
- 占位 op 在 `OperationParser` 析构时才销毁（`:908-923`），因为自定义 parser
  可能在解析过程中保存了 `Value` 引用；
- `resolveOperand` 之后的 `Value` 才保证有最终定义；
- `%x#N` 的 `N` 是**结果号**，不是 SSA 名的组成部分。

## 6. 案例：printer 省略 entry block 签名导致 IR 不可回读

这是本工作区修复过的一个真实问题（upstream issue **#225730**），
它精确地展示了 parse/print 的对称契约。

### 6.1 现象

```mlir
func.func @f(%x : !llvm.ptr) {
  omp.target kernel_type(generic) {
    ^bb0(%arg0: !llvm.ptr):
    %c = arith.constant 10 : i32
    llvm.store %c, %arg0 : i32, !llvm.ptr
    omp.terminator
  }
  return
}
```

```console
$ mlir-opt in.mlir > out.mlir   # rc=0
$ mlir-opt out.mlir             # error: use of undeclared SSA value name
```

第一遍打印出的 `omp.target` region 里，`^bb0(%arg0: !llvm.ptr):` 整行消失，
但 `llvm.store` 仍在用 `%arg1`，于是第二遍解析失败。

### 6.2 根因

`omp.*` 的 region 打印函数统一用：

```cpp
p.printRegion(region, /*printEntryBlockArgs=*/false);
```

省略签名的前提是：entry block 的参数能由 clause 操作数（`map_entries(... -> %arg0)`、
`private(... -> %arg1)` 等）在解析时重建。`printBlockArgClause` 只打印
`BlockArgOpenMPOpInterface` 声明的参数，若 entry block 里存在没有 clause 支撑的参数，
它的定义就会被丢弃，而使用仍然被打印。

### 6.3 修复

在 `printBlockArgRegion`（`mlir/lib/Dialect/OpenMP/IR/OpenMPDialect.cpp`）里比较
“entry block 参数个数”与“clause 定义的参数个数”：

```cpp
bool printEntryBlockArgs =
    region.getNumArguments() != iface.numClauseBlockArgs();
p.printRegion(region, printEntryBlockArgs);
```

- 全部可重建 → 仍省略签名（保持既有输出）；
- 存在无法重建的参数 → 打印 `^bb0(...)` 签名，保证定义可见。

回归测试放进 `mlir/test/Dialect/OpenMP/ops.mlir` 的 roundtrip RUN 行
（`mlir-opt %s | mlir-opt | FileCheck %s`），`CHECK-NEXT: ^bb0(` 锁定签名被打印。

### 6.4 对应的 parser 约束

`parseRegionBody`（`Parser.cpp:2351-2375`）：

```cpp
if (!entryArguments.empty() && !entryArguments[0].ssaName.name.empty()) {
  if (getToken().is(Token::caret_identifier))
    return emitError("invalid block name in region with named arguments");
  ...
}
```

也就是说：**clause 提供命名 entry 参数时，文本里不允许再出现 `^bb0(...)`**。
因此 printer 的两种模式是互斥的：

| parser 看到的文本 | 结果 |
| --- | --- |
| `map_entries(%x -> %arg0 : ...) { ... }` | entry 参数由 clause 创建 |
| `... { ^bb0(%arg0: T): ... }` | entry 参数由签名创建（此时不能有 clause 参数） |

这解释了为什么“无法重建时打印签名”是正确修复，而“同时打印 clause 和签名”会导致
`invalid block name in region with named arguments`。

### 6.5 一般化教训

修改自定义 assembly format 时，先问三个问题：

1. **parser 能重建哪些信息？**（clause、属性、类型）
2. **printer 省略了什么？**（省略的部分是否一定可重建）
3. **无法重建的边界输入**（手写文本、程序化构造的 IR）会打印成什么？

只测“自己生成的 IR”不够，roundtrip 测试必须包含“解析→打印→再解析”的闭环。

## 7. 其它内部细节

| 细节 | 说明 |
| --- | --- |
| properties 的设置时机 | op 创建后才 `setPropertiesFromAttribute`（`Parser.cpp:1624`、`:2231`），因为需要 op 的 properties 存储 |
| generic 语法的固有属性 | 无 `<properties>` 时，普通 attr-dict 里的固有属性会先 `verifyInherentAttrs`（`:1596-1620`） |
| 重复属性 | 自定义 parser 结束后检查 `NamedAttrList::findDuplicate`（`:1672-1677`） |
| `distinct` ID | 同一 ID 必须引用同一属性，跨整个解析会话（`AttributeParser.cpp:1389-1400`） |
| 无并行解析 | 当前实现串行；`parseSourceFileForTool` 只是容器 op 包装 |
| 无错误恢复 | 首个错误即终止；`Token::error` 直接返回 |
| 诊断位置 | `emitWrongTokenError` 会回溯到上一行行尾（跳过空行/注释） |

## 8. 调试建议

| 需求 | 手段 |
| --- | --- |
| 看 op 的通用形式 | `mlir-opt -mlir-print-op-generic` |
| 打印 location | `mlir-opt -mlir-print-debuginfo` |
| 只验证解析/打印 | `mlir-opt %s | mlir-opt`（roundtrip） |
| 禁用解析后 verifier | 在工具里传 `ParserConfig(ctx, /*verifyAfterParse=*/false)` |
| 允许未注册方言 | `-allow-unregistered-dialect`（`Parser.cpp:1558-1570`） |
| 看补全行为 | 通过 `AsmParserCodeCompleteContext` 注入补全点（见 07 章） |

## 9. 版本边界

本文所有行号基于 `5f7f36636abe`。上游仍在演进（例如 parser/printer 的接口、
`AsmParserState` 的 API 都在变化），阅读时以函数名为主线、行号为辅；
“内部契约”一节（占位值、lexer 重置、错误恢复策略）尤其可能随版本调整。
