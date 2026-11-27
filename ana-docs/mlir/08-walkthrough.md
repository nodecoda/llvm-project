# 08 端到端走查

本章用两段紧凑的 MLIR 文本，把前七章的机制串起来。

## 示例 A：block、前向引用与 SSA

```mlir
func.func @loop(%n: index) -> index {
  %c0 = arith.constant 0 : index
  cf.br ^bb1(%c0 : index)
^bb1(%i: index):
  %next = arith.addi %i, %c0 : index
  %cond = arith.cmpi slt, %next, %n : index
  cf.cond_br %cond, ^bb2, ^bb3
^bb2:
  cf.br ^bb1(%next : index)
^bb3:
  return %next : index
}
```

### 步骤 1：入口

```
mlir::parseSourceFile(SourceMgr, Block*, ParserConfig)      // mlir/lib/Parser/Parser.cpp:38
  → isBytecode? 否
  → mlir::parseAsmSourceFile(...)                           // Parser.cpp:2977
      → ParserState state(sourceMgr, config, aliases, ...)  // 初始化 lexer，预读第一个 token
      → TopLevelOperationParser(state).parse(block, loc)    // Parser.cpp:2921
```

`ParserState` 构造时 `Lexer` 已经读了第一个 token（`ParserState.h:58` 的
`curToken(lex.lexToken())`），所以后面所有逻辑都是“当前 token + 一次消费”。

### 步骤 2：顶层分派

`parse` 看到 `func.func`：`bare_identifier`（含 `.`，不是关键字），
进入 `OperationParser::parseOperation`（`Parser.cpp:1275`）：

1. 当前 token 不是 `%`，没有结果名列表；
2. `nameTok.is(bare_identifier)` → `parseCustomOperation`（`:2101`）。

### 步骤 3：名字解析与 op 回调

`parseCustomOperationName`（`:2061`）：

- `RegisteredOperationName::lookup("func.func", ctx)` 命中（`func` 方言已加载/已注册）；
- 取 `getParseAssemblyFn()`，它不是手写 parser，而是 ODS 根据 `assemblyFormat`
  生成的 `FuncOp::parse`。

`parseCustomOperation` 随后：

- 压入该 op 的默认方言（`func.func` 的 `OpAsmOpInterface::getDefaultDialect` 通常为空串）；
- 构造 `CustomOpAsmParser`，调用 `FuncOp::parse(*this, opState)`。

### 步骤 4：函数体 region

生成的 parser 依次消费 `@loop`、`(%n: index)`、`-> index`，遇到 `{` 调
`OpAsmParser::parseRegion`（转发到 `OperationParser::parseRegion`，`:2298`）：

- `entryArguments` 为空，进入普通 region 解析；
- `pushSSANameScope()`；
- 第一个 block 匿名，直接 `parseBlock(block)`（`:2421`）：没有 `^` 标签，
  走 `parseBlockBody`（`:2489`）循环解析 op。

### 步骤 5：`%c0` 与 `cf.br ^bb1`

`%c0 = arith.constant 0 : index`：

- `parseOperation` 先解析 `%c0` 结果名（`ResultRecord`），再 custom 解析；
- 创建 op 后 `addDefinition` 把 `%c0` 登记进当前作用域（`:1356-1364`）。

`cf.br ^bb1(%c0 : index)`：

- `cf.br` 的 assembly format 调 `parseSuccessorAndUseList`（`Parser.cpp:1983`）；
- `parseSuccessor` → `getBlockNamed("^bb1")`（`:2504`）：块还不存在，
  创建空 `Block` 并登记到 `forwardRef`；
- 操作数 `%c0` 走 `resolveSSAUse`，此时已定义，直接绑定。

### 步骤 6：`^bb1(%i: index):`

`parseBlock` 看到 `^bb1`：

- `getBlockInfoByName("^bb1")` 命中之前的**前向引用块**，
  `eraseForwardRef` 成功，复用该 `Block`（`:2450-2475`）；
- 解析参数列表：新块没有参数，`definingExistingArgs == false`，
  为 `%i` 创建 `BlockArgument` 并 `addDefinition`；
- 要求 `:`，进入 block body。

如果 `^bb1` 从未定义，region 结束时的 `popSSANameScope` 会报
`reference to an undefined block`（`:1005-1036`）。

### 步骤 7：`cf.cond_br %cond, ^bb2, ^bb3`

`^bb2`/`^bb3` 同样先建成前向引用块，后面各自被定义时复用。
`cf.br ^bb1(%next : index)` 里的 `%next` 已定义，直接解析。

### 步骤 8：收尾

顶层遇到 `eof` 调 `OperationParser::finalize`（`:927`）：

1. 没有未解析的 SSA 前向引用；
2. 没有延迟 location；
3. `popSSANameScope`：所有 block 都已定义；
4. `verify(module)`（默认开启）：SSA 支配关系、`func.return` 类型等；
5. 把临时 `builtin.module` 里的 `func.func` splice 到调用者 block。

## 示例 B：属性、类型别名与延迟 location

```mlir
!t = tensor<4xf32>
#map = affine_map<(d0)[s0] -> (d0 + s0)>

func.func @attrs(%v: !t, %i: index) -> f32 {
  %c = arith.constant 1.0 : f32
  %0 = "test.generic"(%v) {map = #map, arr = array<i32: 1, 2>} : (!t) -> !t loc(#loc)
  %1 = tensor.extract %0[%i] : !t
  return %1 : f32
}

#loc = loc("test.mlir":1:1)   // 后置定义，触发 location 的延迟解析
```

### 步骤 1：`!t` 与类型别名

`func.func` 扫描签名时 `parseType` 遇到 `!t`：

- `parseExtendedSymbol`（`DialectSymbolParser.cpp:178`）发现没有 `.`、没有紧跟 `<`；
- 查 `typeAliasDefinitions` 命中，直接返回 `tensor<4xf32>`。

如果把 `!t = ...` 挪到函数之后，会在使用处立即报
`undefined symbol alias id 't'`——**类型/属性别名不支持前向引用**，
只有 location alias 支持（见 06 章第 2.3 节）。

### 步骤 2：`#map` 与 `array`

- `#map` 是属性别名，查 `attributeAliasDefinitions` 得到 `AffineMapAttr`；
- `array<i32: 1, 2>` 由 `parseDenseArrayAttr`（`AttributeParser.cpp:939`）解析，
  元素走 `DenseArrayElementParser`，最终生成原始字节形式的 `DenseArrayAttr`。

### 步骤 3：generic op

`"test.generic"(%v) {map = #map, arr = ...} : (!t) -> !t`：

- 首 token 是 `string` → `parseGenericOperation`（`:1540`）；
- `OperationParser::parseGenericOperationAfterOpName`（`:1430`）：
  - 操作数 `(%v)` 记 `UnresolvedOperand`；
  - 属性字典用 `parseAttributeDict`；
  - `: (!t) -> !t` 是 `FunctionType`，用于一次性 resolve 操作数并设置结果类型；
- 构造 op 前会检查“固有属性”的类型兼容性（`:1596-1620`）。

### 步骤 4：`loc(#loc)` 的延迟解析

`#loc` 在函数之后才定义，但这里先使用：

1. `parseTrailingLocationSpecifier`（`:2264`）发现 `#loc` 不含 `.`，调
   `parseLocationAlias`（`:2237`）；
2. 别名表中还没有 `loc`，于是创建
   `OpaqueLoc::get(index, TypeID::get<DeferredLocInfo *>(), UnknownLoc)` 作为哨兵，
   并 `deferredLocsReferences.push_back({tok.getLoc(), "loc"})`；
3. 顶层继续解析，读到 `#loc = loc("test.mlir":1:1)` 时把定义写入属性别名表；
4. 文件尾 `finalize` 遍历整棵 IR，把哨兵替换为真正的
   `FileLineColLoc("test.mlir", 1, 1)`（`:944-975`）。

如果最终没有 `#loc = ...` 定义，会报
`operation location alias was never defined`。默认打印不显示 location，
可用 `mlir-opt -mlir-print-debuginfo` 观察（打印器会为 location 重新生成 `#locN` 别名）。

## 调用栈总览

```
parseSourceFile
└─ parseAsmSourceFile
   └─ TopLevelOperationParser::parse                     // 文件级循环
      ├─ parseAttributeAliasDef / parseTypeAliasDef      // #/!
      ├─ OperationParser::parseOperation                 // 每个 op
      │  ├─ parseCustomOperation / parseGenericOperation
      │  │  └─ Op::parse (dialect 提供)                  // 自定义语法
      │  │     ├─ parseRegion → parseRegionBody → parseBlock
      │  │     └─ parseOperand / resolveOperand           // SSA 延迟绑定
      │  └─ addDefinition                                // 登记结果名
      └─ OperationParser::finalize                       // 未定义检查/延迟 loc/verify
```

下一章：[09-pitfalls-and-internals.md](09-pitfalls-and-internals.md)。
