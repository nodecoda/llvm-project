# 07 工具能力：AsmParserState、代码补全、诊断与资源

`mlir/lib/AsmParser/` 除了构造 IR，还支撑两类工具场景：

- **源码 ↔ IR 映射**（`AsmParserState`）：LSP 的跳转/重命名/高亮、格式化；
- **代码补全**（`AsmParserCodeCompleteContext`）：在解析到一半的文本上给出候选。

两者都是可选的旁路：不传对应指针时解析行为完全不变
（`ParserState.h:81-83`、`ParserState.h:85-87`）。

## 1. AsmParserState

### 1.1 定位

头文件注释明确写着：

> This should generally not be used for traditional compilation.
> —— `mlir/include/mlir/AsmParser/AsmParserState.h:26-29`

它记录的是“文本里每个定义/使用的位置”，而不是语义信息。

### 1.2 记录模型

| 结构 | 内容 | 位置 |
| --- | --- | --- |
| `SMDefinition` | 定义位置 `SMRange` + 所有使用位置 | `AsmParserState.h:35-44` |
| `OperationDefinition` | op 指针、名字范围、整 op 范围、结果组、符号使用 | `:47-79` |
| `ResultGroupDefinition` | 结果起始下标 + 定义位置 | `:48-56` |
| `BlockDefinition` | 块指针、定义、每个块参数的定义 | `:81-94` |
| `AttributeAliasDefinition` / `TypeAliasDefinition` | 别名定义与使用 | `:96-127` |

内部实现（`AsmParserState.cpp:33-87`）用 `DenseMap` 建立
`Operation* → 下标`、`Block* → 下标`，并有：

- `placeholderValueUses`：前向引用占位值的临时使用表，定义出现后迁移；
- `symbolUseScopes`：按 `SymbolRefAttr` 的嵌套层级记录符号使用；
- `SymbolTableCollection`：`finalize` 时解析符号引用（`:89-107`）。

### 1.3 查询 API

| API | 作用 |
| --- | --- |
| `getOpDefs()` / `getOpDef(Operation *)` | 遍历/查询 op 定义信息 |
| `getBlockDefs()` / `getBlockDef(Block *)` | 块与块参数 |
| `getAttributeAliasDefs()` / `getAttributeAliasDef(StringRef)` | `#alias` |
| `getTypeAliasDefs()` / `getTypeAliasDef(StringRef)` | `!alias` |
| `convertIdLocToRange(SMLoc)` | 把 op 结果的 id 位置扩展成名字范围 |

（声明见 `AsmParserState.h:147-179`。）

注意：**没有** `findDefinition/getUses` 这类语义查询；LSP 的“跳转到定义/查找引用”
是在 server 层基于这些位置信息 + IR 的 use 链自行实现的。

### 1.4 填充时机（Parser.cpp 调用点）

| 事件 | 调用 |
| --- | --- |
| 解析开始/结束 | `initialize`（`:903-905`）、`finalize`（`:985-987`） |
| generic op | `finalizeOperationDefinition`（`:1643-1646`） |
| custom op | `startOperationDefinition`（`:2202-2203`）→ 成功后 `finalizeOperationDefinition`（`:1343-1354`、`:1367-1370`） |
| region | `startRegionDefinition`（`:2306-2308`）/ `finalizeRegionDefinition`（`:2318-2320`） |
| block 与参数 | `addDefinition`（`:2348-2349`、`:2374-2375`） |
| SSA 使用 | `resolveSSAUse` 成功路径（`:1139-1140`） |
| 前向引用补齐 | `refineDefinition`（`:1073-1074`） |
| 符号/别名使用 | `AsmParserImpl::parseOptionalSymbolName`（`AsmParserImpl.h:520-525`）、`SymbolRefAttr`（`AttributeParser.cpp:217-218`）、location alias（`Parser.cpp:2244-2245`）、attr/type alias（`DialectSymbolParser.cpp:212-216`） |

因此 `AsmParserState` 是**解析过程的副产品**，不需要二次遍历源码。

## 2. 代码补全

### 2.1 补全 token 的来源

`Lexer` 构造时接收 `AsmParserCodeCompleteContext *`，其
`getCodeCompleteLoc()` 给出一个字节偏移（`Lexer.cpp:57-60`）。补全点会：

- 在主循环起始处直接产出 `Token::code_complete`（`Lexer.cpp:87-88`）；
- 在带前缀标识符/字符串**内部**时，产出只包含补全点之前内容的
  `code_complete` token（`Lexer.cpp:397-404`、`:418-423`），
  以便 parser 用“半个名字”计算候选。

`Token::isCodeCompletionFor(kind)`（`Token.cpp:194-211`）根据 spelling 的首字符
（`% # ^ ! "`）判断“这个补全请求原本是什么 token”。

### 2.2 补全的解析路径

`Parser` 的 `codeComplete*` 系列（`Parser.h:356-367`）与
`OperationParser::codeCompleteSSAUse` / `codeCompleteBlock`（`Parser.cpp:2573`、`:2615`）：

| 场景 | 处理 |
| --- | --- |
| op 名 / 方言名 | `codeCompleteDialectOrElidedOpName`（`:550`）、`codeCompleteOperationName`（`:540`）、`codeCompleteDialectName`（`:535`） |
| SSA 操作数 | 从各隔离作用域收集名字、排序后回填（`:2573`） |
| block 名 | 收集当前作用域的块名（`:2615`） |
| 期望的固定 token | `codeCompleteExpectedTokens`（`:586`）、`codeCompleteOptionalTokens`（`:590`） |
| 方言属性/类型 | `codeCompleteDialectSymbol`（`:606`/`:610`），用 alias 表生成候选 |

**重要约定**：补全函数返回 `failure()` 并**不表示补全失败**，而是“解析到此为止”的信号
（`Parser.h:349-353` 的注释）。顶层调用方看到补全结果后停止解析。

## 3. 诊断

### 3.1 位置

所有诊断位置最终由 `Lexer::getEncodedSourceLocation`（`Lexer.cpp:62`）编码为
`FileLineColLoc(文件名, 行, 列)`。解析器本身不打印诊断，而是调用
`mlir::emitError(Location, ...)`，由 `MLIRContext` 上注册的 diagnostic handler
决定输出/收集方式（命令行工具会安装 `SourceMgrDiagnosticHandler` 带源码行显示）。

### 3.2 两类错误原语

| 原语 | 位置 | 语义 |
| --- | --- | --- |
| `emitError(msg)` | `Parser.cpp:193` | 在当前 token 处报错；EOF 时回退一个字符 |
| `emitError(loc, msg)` | `:239` | 指定位置；若当前 token 是 lexer 的 `error` 则 `abandon()` 避免重复报错 |
| `emitWrongTokenError(msg)` | `:255` | “expected X”：若当前 token 位于行首，回溯到上一行行尾再报 |

`emitWrongTokenError` 的回溯逻辑（`:255-303`）：

1. 跳过纯空行；
2. 若上一行行尾有 `//` 注释（`findCommentStart`，`:205-236` 忽略字符串里的 `//`），
   把诊断位置放到注释之前；
3. 这样“漏写逗号/右括号”这类错误会指向真正该补 token 的位置，而不是下一行开头。

### 3.3 失败传播与清理

- 返回 `nullptr`/`failure()` 即失败；调用方逐层返回，不做恢复；
- `CleanupOpStateRegions`（`:1416-1428`）在 `OperationState` 析构时
  drop 未完成 region 里的 use；
- `OperationParser` 析构时销毁占位 op 与未采用的块（`:908-923`）；
- `AsmParserImpl::emittedError` 保证“发过诊断就不构造 op”（`:2212-2214`）。

## 4. file metadata 与资源

### 4.1 语法

```ebnf
file-metadata ::= "{-#" entry ("," entry)* "#-}"
entry         ::= key ":" value
key           ::= "dialect_resources" | "external_resources"
dialect_resources ::= "{" ns ":" "{" resource-entry* "}" ... "}"
external_resources ::= "{" name ":" "{" resource-entry* "}" ... "}"
resource-entry ::= (bare-id | string) ":" (string | hex-string | ...)
```

`{-#` / `#-}` 是 lexer 的专用 token（见 02 章），只在顶层被识别
（`TopLevelOperationParser::parse`，`Parser.cpp:2921`）。

### 4.2 处理流程

1. `parseFileMetadataDictionary`（`:2819`）只接受两个 key，其它报
   `unknown key '...' in file metadata dictionary`；
2. `dialect_resources`（`:2842`）：
   - 按方言名 `getOrLoadDialect`，方言必须实现 `OpAsmDialectInterface`；
   - 资源名通过 `parseResourceHandle`（`:492`）交给 dialect 的 `declareResource`，
     dialect 可以重命名（`getResourceKey`），结果缓存在
     `SymbolState::dialectResources`；
   - 值交给 `OpAsmDialectInterface::parseResource`。
3. `external_resources`（`:2890`）：
   - 通过 `ParserConfig::getResourceParser(name)` 找宿主注册的 `AsmResourceParser`；
   - 找不到时只发 warning（`ignoring unknown external resources for '...'`）而不是错误。

### 4.3 `ParsedResourceEntry`

`Parser.cpp:2669` 定义，供 dialect/宿主解析值：

| 方法 | 说明 |
| --- | --- |
| `getKey()` | 资源 key |
| `parseAsString()` | 字符串值（`:2697`） |
| `parseAsBlob(allocator)` | 十六进制 blob：前 4 字节是 little-endian 对齐值，其后是数据（`:2705-2747`） |

`parseAsBlob` 会校验对齐值是 2 的幂，并用传入的 allocator 分配内存。
`parseSourceFile` 的 `shared_ptr<SourceMgr>` 重载（`mlir/lib/Parser/Parser.cpp:52`）
存在的目的之一，就是让资源 blob 可以直接引用输入缓冲区，避免大资源复制。

## 5. 性能相关的实现取舍

| 取舍 | 说明 |
| --- | --- |
| token spelling 零拷贝 | `StringRef` 指向 `SourceMgr` 缓冲，字符串只在需要时反转义 |
| 单 token 前瞻 | 无回溯，`OptionalParseResult` 表达“不是这种语法” |
| 延迟 SSA 绑定 | 避免“先读类型再读名字”的顺序限制 |
| 按需加载 dialect | 只有出现 `ns.op` / `!ns.*` / `#ns.*` 才 `getOrLoadDialect` |
| contexte uniquing | 类型/属性/affine map 由 `MLIRContext` 去重，解析器不维护缓存 |
| 无并行解析 | 当前版本全程串行；`parseSourceFileForTool` 只是容器 op 包装 |

下一章：[08-walkthrough.md](08-walkthrough.md)。
