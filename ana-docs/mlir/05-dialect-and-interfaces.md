# 05 方言扩展语法与接口层

MLIR 把“内置语法”和“方言语法”分开：内置类型/属性/操作由 `AsmParser` 直接实现，
方言类型、方言属性、自定义 op 格式则通过稳定接口回调到 dialect/op 代码。
本章讲三件事：

1. `!`/`#` 之后的三态分派（alias / verbose / pretty）；
2. dialect hook 的调用时序（lexer 指针重置、`:` type 的解析时机）；
3. `AsmParserImpl` 如何把内部解析状态桥接成 `OpAsmParser` / `DialectAsmParser`。

## 1. `!` / `#` 的三态分派（`DialectSymbolParser.cpp:178`）

```cpp
template <typename Symbol, typename SymbolAliasMap, typename CreateFn>
static Symbol parseExtendedSymbol(Parser &p, AsmParserState *asmState,
                                  SymbolAliasMap &aliases, CreateFn &&create);
```

输入的 token 已经由 lexer 切成 `exclamation_identifier`（`!foo.bar`）或
`hash_identifier`（`#foo.bar`），`identifier = spelling.drop_front()`。

### 1.1 判别规则

```cpp
auto [dialectName, symbolData] = identifier.split('.');
bool isPrettyName = !symbolData.empty() || identifier.back() == '.';
bool hasTrailingData = p.getToken().is(Token::less) &&
                       identifier.bytes_end() == p.getTokenSpelling().bytes_begin();
if (!hasTrailingData && !isPrettyName) → alias
else if (!isPrettyName)                 → verbose   (ns<...>)
else                                    → pretty    (ns.sym 或 ns.sym<...>)
```

三点容易忽略：

- **`hasTrailingData` 要求 `<` 紧邻标识符末字符**（指针相邻），
  `#foo <x>` 中间有空格时不算 trailing data；
- 名字以 `.` 结尾（`#foo.`）也算 pretty（`identifier.back() == '.'`）；
- alias 名不允许含 `.`（定义时已检查，`Parser.cpp:2763-2766`、`:2796-2799`），
  所以“含 `.` ⇔ dialect 语法”成立。

### 1.2 三种形态

| 形态 | 例子 | `symbolData` 传给 dialect 的内容 |
| --- | --- | --- |
| alias | `#map`、`!mytype` | 不调用 dialect，直接查 `attributeAliasDefinitions` / `typeAliasDefinitions` |
| verbose | `!llvm.ptr<...>`、`#gpu.thread<x>` | `<` 与 `>` **之间**的原文（去掉尖括号） |
| pretty | `!llvm.ptr`、`!my.type<"x">` | `.` 之后的原文；若紧跟 `<...>`，body 包含 `sym<...>`（**保留尖括号**） |

alias 未定义时报 `undefined symbol alias id 'x'`（`:200-208`）。
alias 解析成功会记录 use（`addAttrAliasUses` / `addTypeAliasUses`），供 LSP 使用。

### 1.3 `<...>` 体的扫描（`parseDialectSymbolBody`，`:66`）

dialect 体不是 MLIR 语法，而是“括号平衡的原始文本”，扫描规则：

- 维护 `nestedPunctuation` 栈：`< [ ( {` 入栈，`> ] ) }` 校验匹配后出栈；
- `->` 作为一个整体跳过（`:112-115`），避免把 `>` 当作右尖括号；
- 遇到 `"` 交回 lexer 处理字符串（`resetToken(curPtr - 1)`，`:119-129`），
  这样字符串里的 `>`/`}` 不会破坏平衡；
- 遇到 `\0` 或缓冲区末尾仍不平衡时报错（`:86-98`）；
- 若扫描中遇到补全点，则提前结束并标记 `isCodeCompletion`（`:84-88`）。

注意它只保证括号平衡，**不做语法解析**；真正的语法由 dialect 回调在重定位后的
lexer 上完成。

## 2. dialect hook 的调用时序

### 2.1 属性（`parseExtendedAttr`，`:258`）

```cpp
Attribute Parser::parseExtendedAttr(Type type) {
  Attribute attr = parseExtendedSymbol<Attribute>(...,
    [&](StringRef dialectName, StringRef symbolData, SMLoc loc) -> Attribute {
      // 1) 可选尾随 ": type" 在 hook 之前解析
      Type attrType = type;
      if (consumeIf(Token::colon) && !(attrType = parseType()))
        return Attribute();

      // 2) 加载方言（可能触发注册）
      if (Dialect *dialect = builder.getContext()->getOrLoadDialect(dialectName)) {
        // 3) 把 lexer 临时指到 symbolData，调用 dialect
        const char *curLexerPos = getToken().getLoc().getPointer();
        resetToken(symbolData.data());
        CustomDialectAsmParser customParser(symbolData, *this);
        Attribute attr = dialect->parseAttribute(customParser, attrType);
        resetToken(curLexerPos);   // 4) 恢复
        return attr;
      }

      // 5) 未注册方言 → OpaqueAttr（允许 -allow-unregistered-dialect 的场景）
      return OpaqueAttr::getChecked(...);
    });

  // 6) 若调用方给了期望类型，做 TypedAttr 一致性检查
  if (type && typedAttr && typedAttr.getType() != type) { ... }
  return attr;
}
```

关键点：

- `: type` 属于**外层语法**，dialect 的 `parseAttribute` 看不到它；
- `resetToken` 之后，dialect 里的错误位置都相对于 symbol body；
- 恢复 lexer 位置后，外层继续解析（例如 op 的后续 token）；
- 未注册方言时构造 `OpaqueAttr`，其类型由尾随 `: type` 或 `NoneType` 决定。

### 2.2 类型（`parseExtendedType`，`:305`）

同构，但**没有**尾随 `: type`，未注册时构造 `OpaqueType`。

### 2.3 独立字符串 API

`mlir::parseAttribute(StringRef, MLIRContext *, Type, size_t *numRead, ...)` 与
`mlir::parseType(...)`（`DialectSymbolParser.cpp:337-385`）用一个一次性的
`SourceMgr`/`ParserState` 复用同一套代码：

- 给了 `numRead` 时允许“只解析前缀”，否则要求消费到 EOF（`:363-368`）；
- 这是 `parseAttribute("42 : i8", ctx)` 这类工具 API 的实现方式。

## 3. `AsmParserImpl`：接口与内部状态的桥

### 3.1 结构

`AsmParserImpl<BaseT>`（`AsmParserImpl.h:28`）是模板混入：

```cpp
template <typename BaseT>
class AsmParserImpl : public BaseT {
  SMLoc nameLoc;     // 当前 op/符号名位置，用于诊断
  Parser &parser;    // 底层解析器（token 游标、builder、diagnostics）
  bool emittedError; // 自定义 parser 是否已发过诊断
};
```

它**不拥有** SSA 表/块表；所有状态仍在 `ParserState`/`OperationParser`。
桥接就是“把接口调用翻译成对 `parser` 的调用”。

### 3.2 接口 → 实现 → 底层

| `OpAsmParser` 接口 | 实现位置 | 委托到 |
| --- | --- | --- |
| `getNameLoc` / `getCurrentLocation` | `AsmParserImpl.h:35`、`:56` | `parser.getToken().getLoc()` |
| `emitError` | `AsmParserImpl.h:45` | `Parser::emitError`（并置 `emittedError`） |
| `getBuilder` | `AsmParserImpl.h:52` | `Parser::builder` |
| `parseArrow`/`parseLBrace`/…（token 族） | `AsmParserImpl.h:70-257` | `Parser::parseToken` / `consumeIf` |
| `parseKeyword` / `parseOptionalKeyword` | `AsmParserImpl.h:335-421` | token 拼写比较 + `consumeToken` |
| `parseAttribute` | `AsmParserImpl.h:428` | `Parser::parseAttribute`（`AttributeParser.cpp:49`） |
| `parseOptionalAttribute`（4 个重载） | `AsmParserImpl.h:456-471` | `Parser::parseOptionalAttribute`（`AttributeParser.cpp:245`） |
| `parseOptionalAttrDict` | `AsmParserImpl.h:474` | `Parser::parseAttributeDict`（`:296`） |
| `parseOptionalSymbolName` | `AsmParserImpl.h:512` | 读 `at_identifier`，并写 `asmState` |
| `parseResourceHandle` | `AsmParserImpl.h:535` | `Parser::parseResourceHandle`（`Parser.cpp:492`） |
| `parseType` / `parseOptionalType` / type-list | `AsmParserImpl.h:550-594` | `TypeParser.cpp:77` / `:31` / `:104` |
| `pushCyclicParsing` / `popCyclicParsing` | `AsmParserImpl.h:607-613` | `ParserState::cyclicParsingStack` |
| `parseKeywordOrCompletion` / `codeCompleteExpectedTokens` | `AsmParserImpl.h:621-635` | `Parser::codeComplete*` |
| `parseOperand` / `parseOperandList` | `Parser.cpp:1751-1808`（`CustomOpAsmParser`） | `OperationParser::parseSSAUse`（`:1105`） |
| `resolveOperand` | `Parser.cpp:1811-1818` | `OperationParser::resolveSSAUse`（`:1133`） |
| `parseRegion` / `parseOptionalRegion` | `Parser.cpp:1927-1963` | `OperationParser::parseRegion`（`:2298`） |
| `parseArgument` / `parseArgumentList` | `Parser.cpp:1887-1922` | operand + 可选 type/attr-dict/loc |
| `parseAffineMapOfSSAIds` / `parseAffineExprOfSSAIds` | `Parser.cpp:1821-1875` | `AffineParser.cpp` |

为什么操作数要单独在 `CustomOpAsmParser` 里实现？
因为 `OpAsmParser` 的语义是“op 自定义格式”：`parseOperand` 只产生
`UnresolvedOperand`，`resolveOperand` 才把类型补上并变成 `Value`（见 03 章第 6 节）。
`DialectAsmParser` 侧不需要这套延迟机制，所以直接复用 `AsmParserImpl` 的通用实现。

### 3.3 `emittedError` 的意义

自定义 parser 可能返回 success 但已经 `emitError`（例如“报错但继续读完 token”）。
`CustomOpAsmParser::parseOperation` 之后会检查 `didEmitError()`，有错就不构造 op
（`Parser.cpp:2212-2214`），避免产生“带诊断的成功 IR”。

## 4. 注册与扩展点

| 扩展点 | 声明位置 | 用途 |
| --- | --- | --- |
| `OperationName::getParseAssemblyFn` | `mlir/include/mlir/IR/OperationSupport.h` | ODS 生成的 `Op::parse` |
| `Dialect::getParseOperationHook` | `mlir/include/mlir/IR/Dialect.h` | 未注册/动态 op 的解析入口 |
| `Dialect::parseAttribute` / `parseType` | `mlir/include/mlir/IR/Dialect.h` | 方言属性/类型 |
| `OpAsmOpInterface::getDefaultDialect` | `OpImplementation.h` | 省略方言前缀时的默认方言 |
| `OpAsmDialectInterface` | `mlir/include/mlir/IR/OpAsmDialectInterface.td` | resource handle（`declareResource`/`getResourceKey`/`parseResource`）等 |
| `AsmParserState` | `mlir/include/mlir/AsmParser/AsmParserState.h` | 工具侧源码映射（非编译主流程） |
| `ParserConfig::attachResourceParser` | `mlir/include/mlir/IR/AsmState.h:504` | `external_resources` 的宿主解析器 |

**边界**：`mlir/lib/AsmParser/` 里的 `Parser`、`OperationParser`、`CustomOpAsmParser`、
`TopLevelOperationParser` 都是私有实现；dialect/op 代码只应依赖
`OpAsmParser`/`DialectAsmParser`/`Dialect` 这些公共接口，不要假设占位值、
`resultIDs`、lexer 重置等内部细节。

## 5. 示例

```mlir
// 1) 内置类型 + dialect 类型（pretty 形式，无 <...>）
%p = llvm.mlir.undef : !llvm.ptr

// 2) verbose 形式：尖括号里是原始文本
"test.op"() : () -> !my.thread<x>

// 3) dialect 属性，尾随 ": type" 由外层解析
"test.op"() {attr = #my.attr<"payload"> : i32} : () -> ()

// 4) alias：无 '.'、无紧跟的 '<'
#map = affine_map<(d0) -> (d0 + 1)>
%m = affine.apply #map(%i)
```

下一章：[06-affine-and-location.md](06-affine-and-location.md)。
