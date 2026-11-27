# 03 语法分析主干

## 1. 类型层次

```
Parser                        (Parser.h:24)          token 原语 + 内置类型/属性/location/affine
  └── OperationParser         (Parser.cpp:622)      SSA/block/region/op + 前向引用
  └── AffineParser            (AffineParser.cpp:59) 仿射子解析器（复用 Parser 的 token 原语）
  └── TopLevelOperationParser (Parser.cpp:2634)     文件级循环 + 别名/元数据
  └── CustomOpAsmParser       (Parser.cpp:1651)     AsmParserImpl<OpAsmParser>，转调 op 的 parseAssembly
```

`Parser` 只保存一个 `ParserState &state`（`Parser.h:369-372` 明确要求：子类不要加
非平凡状态，统一放 `ParserState`），因此各子解析器可以廉价地反复构造。

## 2. Token 原语与“可选解析”协议

| 原语 | 语义 |
| --- | --- |
| `getToken()` / `getTokenSpelling()` | 当前 token，不消费 |
| `consumeToken(kind)` | 断言当前是 `kind` 并前进 |
| `consumeIf(kind)` | 是则消费并返回 true |
| `parseToken(kind, msg)` | 不是则调用 `emitWrongTokenError(msg)` |
| `parseCommaSeparatedList(Delimiter, fn, ctxMsg)` | 支持 `None/Paren/OptionalParen/Square/OptionalSquare/LessGreater` 等分隔符 |
| `parseCommaSeparatedListUntil(right, fn, allowEmpty)` | 直到右边界 token |

三类返回值贯穿全文：

- `ParseResult`：成功/失败（失败时必已发诊断）；
- `OptionalParseResult`：`std::nullopt` 表示“当前语法不适用”，不算错误；
- 指针/值类型（`Type`、`Attribute`、`Operation *` 等）：空值表示失败。

这套协议使得解析器在自定义 op 的 assembly format 里可以写
`if (parser.parseOptionalKeyword("foo")) ...` 之类的试探，而不需要回溯。

## 3. 顶层驱动

```cpp
// Parser.cpp:2921
ParseResult TopLevelOperationParser::parse(Block *topLevelBlock, Location parserLoc) {
  OwningOpRef<ModuleOp> topLevelOp(ModuleOp::create(parserLoc));
  OperationParser opParser(state, topLevelOp.get());
  while (true) {
    switch (getToken().getKind()) {
    default:                      opParser.parseOperation();          // 普通操作
    case Token::eof:              opParser.finalize(); /* splice */    // 结束
    case Token::error:            return failure();                   // lexer 已报错
    case Token::hash_identifier:  parseAttributeAliasDef();           // #map = ...
    case Token::exclamation_identifier: parseTypeAliasDef();          // !t = ...
    case Token::file_metadata_begin: parseFileMetadataDictionary();   // {-# ... #-}
    }
  }
}
```

要点：

- 解析期间所有 op 先挂在一个临时 `builtin.module`（`topLevelOp`）下，
  `finalize()` 成功后把 block 里的 op splice 到调用者给的 block（`:2941-2944`），
  这样失败时可以直接丢弃整个临时 module。
- 文件级语法有四种，靠首 token 区分：`#`（属性别名）、`!`（类型别名）、
  `{-#`（file metadata）、其它（操作）。
- 顶层 `builtin.module` 的 location 是 `FileLineColLoc(文件名, 0, 0)`（`:2982-2985`）。

## 4. 操作解析

### 4.1 结果名列表

```ebnf
operation      ::= op-result-list? (generic-operation | custom-operation) trailing-location?
op-result-list ::= op-result ("," op-result)* "="
op-result      ::= ssa-id (":" integer-literal)?
```

`parseOperation`（`Parser.cpp:1275`）先尝试解析 `%0 =` / `%0:2, %1 =` 这类前缀，
记录 `ResultRecord{name, numResults, loc}`，再根据下一个 token 分派：

- `bare_identifier` 或关键字 → **custom**（op 自定义/声明式格式）；
- `string` → **generic**（`"dialect.op"(...)`）；
- 其它 → `expected operation name in quotes`。

解析成功后校验“声明的结果数 == 实际结果数”，再按结果组调用 `addDefinition`
把每个结果登记进当前 SSA 作用域（`:1339-1365`）。

### 4.2 generic 形式

```ebnf
generic-operation ::= string-literal "(" ssa-use-list? ")"
                     successor-list?
                     region-list?
                     attr-dict?
                     properties?
                     ":" function-type
properties ::= "<" attribute ">"
region-list ::= "(" region ("," region)* ")"
```

实现 `parseGenericOperationAfterOpName`（`Parser.cpp:1430`）：

1. 操作数列表只存 `UnresolvedOperand`；
2. successor 只在 op 具有 `IsTerminator` trait 时允许（`:1455-1458`），
   否则报 `successors in non-terminator`；
3. properties 用 `<` attr `>` 表示；若 op 已支持 properties 但文本里只有普通属性字典，
   会先校验“固有属性”类型（`:1596-1620` 的兼容逻辑）；
4. region 列表里每个 region 以 `topLevelOp` 为 parent 临时创建（`:1476-1490`）；
5. 解析 `: function-type` 得到操作数/结果类型，校验个数后一次性 resolve 所有操作数
   （`:1529-1535`）；
6. `opBuilder.create(result)` 后解析尾随 `loc(...)`，最后再 `setPropertiesFromAttribute`
   （properties 必须等 op 创建后设置，因为需要 op 的 properties 存储）。

### 4.3 custom 形式

分两步：

**名字解析**（`parseCustomOperationName`，`Parser.cpp:2061`）：

1. 接受 `bare_identifier` 或关键字（关键字可作省略方言前缀的 op 名，如 region 里的
   `terminator`）；
2. `RegisteredOperationName::lookup` 命中则直接返回；
3. 否则按 `.` 切分：无前缀时用 `defaultDialectStack.back()` 拼成 `dialect.name`；
4. `getOrLoadDialect`，返回 `OperationName`（可能仍未注册）。

**语法解析**（`parseCustomOperation`，`:2101`）：

1. 从 `RegisteredOperationName::getParseAssemblyFn()` 取 `Op::parse`；
   未注册 op 则走 `Dialect::getParseOperationHook(opName)`；
   两者都没有时报 `custom op 'x' is unknown`，若方言都不存在则列出所有可用方言
   （`:2125-2186`，`(*)` 标记 loaded-but-unregistered）；
2. 把该 op 的默认方言压入 `defaultDialectStack`（`:2188`），离开时弹出；
3. 构造 `CustomOpAsmParser`（`AsmParserImpl<OpAsmParser>` 的具体子类），
   调用 `parseAssembly(*this, opState)`；
4. 检查属性重复（`:1672-1677`）、`didEmitError()`；
5. `opBuilder.create(opState)` 后解析尾随 location、设置 properties（`:2200-2235`）。

## 5. Region 与 Block

### 5.1 region

```ebnf
region        ::= "{" block* "}"
region-body   ::= block? block*
```

`parseRegion`（`Parser.cpp:2298`）与 `parseRegionBody`（`:2325`）：

1. 解析 `{`；空 region（`{}` 且没有 entry arguments）直接结束；
2. `pushSSANameScope(isIsolatedNameScope)`：`IsolatedFromAbove` 的 op 会让其 region
   拥有独立的 SSA 名字空间，内部可复用外部名字；
3. 第一个 block 允许**匿名**（没有 `^bb0:` 标签），由解析器直接创建；
4. **entry arguments 两种来源**：
   - `entryArguments` 非空且带名字（来自 clause 等自定义语法）：
     直接给 entry block 添加参数、登记定义，并且**禁止**文本再出现 `^bb0(...)`，
     否则报 `invalid block name in region with named arguments`；
   - 否则按普通语法解析 `^bb0(%arg: type):`；
5. 后续 block 用 `parseBlock` 循环解析，直到 `}`；
6. `popSSANameScope()` 检查未定义的 block 前向引用（`:1005-1036`）。

> 这就是 parser 与 printer 的接口契约：如果 printer 用 clause 操作数“重建” entry block
> 参数，就不能同时打印 block 签名；反之若参数无法由 clause 重建，就必须打印签名
> （参见 09 章第 6 节的真实 bug）。

### 5.2 block

```ebnf
block           ::= block-label? operation*
block-label     ::= block-id block-arg-list? ":"
block-id        ::= "^" suffix-id
block-arg-list  ::= "(" ssa-id-and-type-list? ")"
ssa-id-and-type ::= ssa-id ":" type ("loc" "(" location ")")?
```

`parseBlock`（`Parser.cpp:2421`）：

- entry block 可省略标签；有标签时 `getBlockInfoByName` 查表：
  - 未见过 → 新块；
  - 已是前向引用 → 复用并 `eraseForwardRef`；
  - 已定义 → `redefinition of block '^bb0'`。
- 标签后可选参数列表（`parseOptionalBlockArgList`），再要求 `:`；
- 参数列表对“已有参数的 entry block”走 **definingExistingArgs** 模式：
  不再创建参数，只校验个数与类型一致（`:2524-2570`），这样 clause 提供的参数
  可以在文本里重新命名。

### 5.3 successor

```ebnf
successor      ::= "^" suffix-id
successor-list ::= "[" successor ("," successor)* "]"
```

`parseSuccessor`（`:1380`）只看 `caret_identifier`；不存在的块名先建空块并登记
前向引用（`getBlockNamed`，`:2504`），region 结束时统一检查。

## 6. SSA 名、作用域与前向引用

### 6.1 使用与定义

```ebnf
ssa-use     ::= "%" suffix-id ("#" integer-literal)?   // 结果号
ssa-use-and-type ::= ssa-use ":" type
```

`parseSSAUse`（`Parser.cpp:1105`）只填 `UnresolvedOperand{name, number, location}`；
类型在 `parseSSADefOrUseAndType`（`:1180`）或 `resolveOperand` 时给出。

`resolveSSAUse`（`:1133`）的逻辑：

1. 该名字/结果号已有定义：
   - 类型相同 → 直接返回，并记录一次 use（若启用 `asmState`）；
   - 类型不同 → 报 `use of value 'x' expects different type than prior uses`，
     并在先前使用处 attach note；
2. 尚未定义：
   - 若名字已有定义但结果号越界 → `reference to invalid result number`；
   - 否则创建**前向引用占位值**并登记。

### 6.2 占位值

`createForwardRefPlaceholder`（`:1242`）创建的是一个
`builtin.unrealized_conversion_cast` 操作的唯一结果（`:1249`），原因很实际：
需要一个带 def/use 链的 `Value`，而 `Value` 不能凭空存在。占位值记录在
`forwardRefPlaceholders` 中，首次使用位置用于诊断。

`addDefinition`（`:1039`）在真实定义出现时：

- 若已有非占位定义 → `redefinition of SSA value`；
- 若类型与先前使用不一致 → 报错并 attach note；
- 若之前是占位值 → `replaceAllUsesWith(真实值)`、清理占位记录，
  若启用 `asmState` 则 `refineDefinition` 修正源码映射（`:1068-1074`）。

### 6.3 作用域

`pushSSANameScope(isIsolated)`（`:995`）维护两个栈：

- `blocksByName` / `forwardRef`：块名前向引用按 region 作用域隔离；
- `isolatedNameScopes`：每个 `IsolatedFromAbove` 边界一个新作用域，
  内部再按 region 嵌套压栈；`popSSANameScope` 时删除本层定义的名字。

因此：

```mlir
func.func @f(%x: i32) {
  // 这里 %x 可见
  omp.parallel {
    // omp.parallel 是 IsolatedFromAbove，内部 %x 不可见
  }
}
```

### 6.4 `finalize`

`OperationParser::finalize`（`:927`）按顺序做：

1. 检查未解析的 SSA 前向引用，按源码位置排序后逐个报
   `use of undeclared SSA value name`；
2. 遍历整棵 IR，把 location 里的 `OpaqueLoc` 哨兵替换成真正的 location alias 值
   （`:944-975`）；
3. `popSSANameScope()`：检查未定义的 block 前向引用；
4. 若 `config.shouldVerifyAfterParse()`，执行完整的 `verify(topLevelOp)`；
5. 收尾 `asmState`。

## 7. 诊断与错误路径

| 函数 | 行为 |
| --- | --- |
| `Parser::emitError(msg)` | 在 `curToken` 位置报错；EOF 时位置回退一个字符（`:193-201`） |
| `Parser::emitError(loc, msg)` | 编码为 `FileLineColLoc`；若当前 token 是 lexer 的 `error`，则 `abandon()` 避免重复报错（`:239-248`） |
| `Parser::emitWrongTokenError(msg)` | “期望 X 但看到 Y”：若当前 token 在行首，回溯到上一行行尾（跳过空行与行尾注释）再报错（`:255-303`） |

设计取舍：

- **无错误恢复**：`Token::error` 直接终止（`:2948-2952`）。
- **失败即清理**：`CleanupOpStateRegions`（`:1411`）在 `OperationState` 析构时
  drop 所有 region 内的 use，避免构造失败留下悬垂引用；
  `OperationParser` 析构时销毁占位 op 与未采用的块（`:908-923`）。
- **诊断即失败**：接口层（`AsmParserImpl`）用 `emittedError` 标记，
  即使自定义 parser 返回 success，只要发过诊断就整体失败（`Parser.cpp:2212-2214`）。

## 8. 小结

主干可以概括为：

```
token 游标 + 一个模块级 OpBuilder
  ├─ 解析 result 名 → 分派 generic/custom
  ├─ custom: 名字解析 → dialect/op 回调 → 构造 op → 尾随 loc/properties
  ├─ region: 名字作用域 + entry args（clause 或签名） + block 循环
  ├─ SSA: 先记名，后绑定；未定义先占位，定义后 RAUW
  └─ 文件尾: 未定义检查 → 延迟 location → verify
```

下一章：[04-types-and-attributes.md](04-types-and-attributes.md)。
