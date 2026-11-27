# 01 总体结构

## 1. 解析器在 MLIR 中的位置

MLIR 的文本格式是**面向 IR 的序列化格式**，不是通用编程语言。解析器不构建语法树，
而是单遍扫描、边解析边用 `OpBuilder` 创建 `Operation`/`Block`/`Region`
（`Parser.cpp:2921` 的顶层循环、`:1275` 的 `parseOperation`）。因此：

- 没有 AST 层，也没有“先建树再降级”的阶段；
- 前向引用（SSA 名、block 名、location alias）靠**占位符 + 事后回填**解决；
- 一旦出错立即返回 `failure()`，不做错误恢复（`Parser.cpp:2948-2952` 注释明确说明）。

## 2. 调用链与库边界

公共入口不在 `mlir/lib/AsmParser/`，而在独立的 `MLIRParser` 库：

```
mlir/lib/Parser/Parser.cpp:38      mlir::parseSourceFile(const SourceMgr &, Block *, ParserConfig &)
        ├─ isBytecode(buf)  → readBytecodeFile(...)      // 二进制格式，另一套实现
        └─ 否则            → mlir::parseAsmSourceFile(...)   // Parser.cpp:2977
                                  └─ TopLevelOperationParser(state).parse(block, loc)  // :2921
```

`parseSourceFile<ModuleOp>` 等模板重载（`mlir/include/mlir/Parser/Parser.h:38-82`）
只是把解析结果包装成容器 op：若解析块里恰好是单个目标 op 就直接复用，
否则新建容器并把 op splice 进去，再 `verifyInvariants()`。

`ParserConfig`（`mlir/include/mlir/IR/AsmState.h:469-528`）携带：

| 配置 | 作用 | 使用点 |
| --- | --- | --- |
| `MLIRContext *context` | dialect 加载、attr/type uniquing、builder | `Parser.h:33-34`、`ParserState.h:52-58` |
| `verifyAfterParse`（默认 true） | 解析完成后是否 `verify(topLevelOp)` | `Parser.cpp:981-983` |
| `resourceParsers` / `fallbackResourceMap` | `{-# external_resources #-}` 的自定义解析器 | `Parser.cpp:2890-2918` |
| `BytecodeReaderConfig` | 二进制读取配置（文本路径不使用） | `mlir/lib/Parser/Parser.cpp:48-49` |

## 3. 分层结构

```
                         ┌──────────────────────────────┐
  源文本 ──► Lexer ──────►│ Token 流（kind + spelling）  │
                         └──────────────┬───────────────┘
                                        ▼
                ┌────────────────────────────────────────────┐
                │ Parser（Parser.h:24）                      │
                │  · token 游标 / 错误诊断                    │
                │  · 类型、属性、location、affine、补全       │
                └──────────────┬─────────────────────────────┘
                               ▼
        ┌──────────────────────────────────────────────────────┐
        │ OperationParser（Parser.cpp:622）                    │
        │  · SSA 名作用域、前向引用、block/region              │
        │  · generic/custom op 解析                            │
        └───────┬───────────────────────────────┬──────────────┘
                ▼                               ▼
   CustomOpAsmParser                TopLevelOperationParser
   (AsmParserImpl<OpAsmParser>)      (Parser.cpp:2634)
   把请求转发给 op 的 parseAssembly   文件级：#alias/!alias/{-# #-}
```

各层职责：

| 层 | 关键类型 | 职责 | 不负责 |
| --- | --- | --- | --- |
| 词法 | `Lexer`、`Token` | 切 token、字符串转义、数字切分、源码位置编码 | 语法合法性 |
| 基础语法 | `Parser` | token 消费、逗号列表、内置类型/属性/location/affine、补全钩子 | SSA 名、block、op |
| 结构语法 | `OperationParser` | 操作、region、block、SSA 定义/使用、前向引用、延迟 location | dialect 自定义语法 |
| 文件级 | `TopLevelOperationParser` | 顶层循环、别名定义、file metadata 字典 | 单个 op 的内容 |
| 扩展适配 | `AsmParserImpl<BaseT>`、`CustomOpAsmParser`、`CustomDialectAsmParser` | 把内部状态桥接成 `OpAsmParser`/`DialectAsmParser` 接口 | 解析策略本身 |
| 工具旁路 | `AsmParserState`、`AsmParserCodeCompleteContext` | 记录源码映射、生成补全候选 | 影响 IR 构造 |

## 4. 文件与职责

| 文件 | 主要内容 | 关键入口 |
| --- | --- | --- |
| `TokenKinds.def` | 记号表（宏展开生成 enum 与 switch） | — |
| `Token.h/.cpp` | `Token` 值类型与字面量解码 | `getStringValue`、`getUInt64IntegerValue` |
| `Lexer.h/.cpp` | 手写扫描器 | `lexToken`（`Lexer.cpp:80`） |
| `ParserState.h` | 全局解析状态 | `SymbolState`、`ParserState` |
| `Parser.h` | 基础语法原语声明 | `parseToken`、`parseType`、`parseAttribute` |
| `Parser.cpp` | 结构语法 + 顶层驱动 + 公共入口 | `parseOperation`（`:1275`）、`finalize`（`:927`）、`parseAsmSourceFile`（`:2977`） |
| `AsmParserImpl.h` | 接口适配模板 | `AsmParserImpl<OpAsmParser>`（`:28`） |
| `TypeParser.cpp` | 类型 | `parseType`（`:77`） |
| `AttributeParser.cpp` | 属性 | `parseAttribute`（`:49`） |
| `DialectSymbolParser.cpp` | `#`/`!` 分派 | `parseExtendedAttr`（`:258`）、`parseExtendedType`（`:305`） |
| `AffineParser.cpp` | 仿射 | `parseAffineMapReference` 等 |
| `LocationParser.cpp` | location | `parseLocationInstance`（`:202`） |
| `AsmParserState.cpp` | 源码映射状态 | `finalizeOperationDefinition` 等 |

## 5. 解析期状态

`ParserState`（`ParserState.h:52-97`）是所有子解析器共享的**唯一可变状态**：

```cpp
struct ParserState {
  const ParserConfig &config;
  Lexer lex;                 // 当前 lexer（含指向源码的游标）
  Token curToken, lastToken; // 一个 token 的前瞻即可
  SymbolState &symbols;      // 文件级符号：attr/type alias、资源、distinct attr
  SetVector<const void *> cyclicParsingStack; // 可变 attr/type 递归保护
  AsmParserState *asmState;                   // 可选：工具侧映射
  AsmParserCodeCompleteContext *codeCompleteContext; // 可选：补全
  SmallVector<StringRef> defaultDialectStack{"builtin"}; // 省略方言前缀时的默认方言
};
```

`SymbolState`（`ParserState.h:28-43`）保存文件级符号：

- `attributeAliasDefinitions` / `typeAliasDefinitions`：`#map`、`!t` 这类别名；
- `dialectResources`：`dialect_resources` 段里各 dialect 的资源句柄缓存；
- `distinctAttributes`：`distinct[ID]<...>` 按整数 ID 去重。

`OperationParser`（`Parser.cpp:622-892`）额外持有 SSA/块状态：

| 成员 | 作用 |
| --- | --- |
| `isolatedNameScopes` | 隔离名字作用域栈，内含 `StringMap<SmallVector<ValueDefinition,1>>` |
| `blocksByName` / `forwardRef` | 块名表与前向块引用（按 region 作用域） |
| `forwardRefPlaceholders` | 未定义 SSA 名的占位值 → 首次使用位置 |
| `forwardRefOps` | 占位 op 集合，解析器析构时才销毁 |
| `deferredLocsReferences` | 未定义 location alias 的延迟回填表 |
| `opBuilder` / `topLevelOp` | 构造 IR 的 builder 与临时顶层 `builtin.module` |

## 6. 六个设计原则

1. **单遍、无回溯**：`OperationParser` 只有一个 token 前瞻；需要“先看再决定”的地方
   用 `OptionalParseResult`（`std::nullopt` = 不是这种语法），而不是回退。
   例外是少数直接改 lexer 指针的场景（如维度列表 `2x3xf32`，`TypeParser.cpp:622`）。
2. **按需加载 dialect**：自定义 op / `!`/`#` 语法出现时才 `getOrLoadDialect`
   （`Parser.cpp:2096`、`DialectSymbolParser.cpp:269`）。
3. **SSA 延迟绑定**：名字先记 `UnresolvedOperand{name, number, location}`，
   类型稍后由 `resolveOperand` 提供；未定义则建占位值。
4. **默认方言栈**：不带 `.` 的 op 名在 `defaultDialectStack.back()` 中解析
   （顶级是 `builtin`，所以 `module { ... }` 可用；`OpAsmOpInterface::getDefaultDialect`
   会在进入该 op 的 region 时压栈，`Parser.cpp:2188-2191`）。
5. **解析即校验**：能构造的地方都用 `getChecked<>` 或显式检查（类型合法、attr 类型匹配、
   重复属性），最后再按需 `verify(topLevelOp)`。
6. **错误即终止**：`emitError` / `emitWrongTokenError` 后返回空值；
   `CleanupOpStateRegions`（`Parser.cpp:1416`）在失败路径上清理未定义 use，防止悬垂引用。

## 7. 与 printer 的契约

自定义 assembly format 是 **parse/print 成对**定义的（ODS 的 `assemblyFormat` 字段会
同时生成 `parse`/`print`）。这意味着：

- `Op::parse` 里“省略”的信息必须在 `Op::print` 里以可被重新解析的形式出现；
- 例如 region 的 entry block 参数若由 clause 操作数重建，printer 就不能再打印签名，
  反之若有无法重建的参数，就必须打印签名（否则打印结果无法回读）。
  这个契约在本工作区的 `omp.target` 修复中给出了一个真实反例（见
  [09-pitfalls-and-internals.md](09-pitfalls-and-internals.md) 第 6 节）。
