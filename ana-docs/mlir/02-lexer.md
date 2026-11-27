# 02 词法分析（Lexer / Token）

## 1. Token 模型

`Token` 是值类型，只有两个字段（`Token.h:124-130`）：

```cpp
class Token {
  Kind kind;          // enum，由 TokenKinds.def 宏展开生成
  StringRef spelling; // 指向 SourceMgr 内存缓冲，零拷贝
};
```

- `spelling` 永远指向源码缓冲区，字符串内容**不预先解码**；解码发生在
  `Token::getStringValue()`、`getSymbolReference()` 等辅助函数里（`Token.cpp`）。
- `Token::getLoc()` / `getLocRange()` / `getEndLoc()` 由 `spelling` 推导
  `SMLoc`/`SMRange`，用于诊断与 IR 的 location。

`TokenKinds.def` 用“可多次 include 的宏表”定义记号，共五类：

| 宏 | 例子 | 生成 |
| --- | --- | --- |
| `TOK_MARKER` | `eof`、`error`、`code_complete` | 无拼写的哨兵 |
| `TOK_IDENTIFIER` | `bare_identifier`、`at_identifier`、`hash_identifier`、`percent_identifier`、`caret_identifier`、`exclamation_identifier` | 带前缀的标识符 |
| `TOK_LITERAL` | `integer`、`floatliteral`、`string`、`inttype` | 字面量 |
| `TOK_PUNCTUATION` | `arrow "->"`、`ellipsis "..."`、`file_metadata_begin "{-#"` | 固定拼写标点 |
| `TOK_KEYWORD` | `func`、`memref`、`affine_map`、`true`/`false` … | `kw_<name>` |

两个使用点：

- `Token.h:22-29`：生成 `Token::Kind` 枚举；
- `Token.cpp:168-192`：生成 `getTokenSpelling` / `isKeyword` 的 switch。

注意：关键字表是**全局固定**的（`TokenKinds.def` 的 `TOK_KEYWORD` 段），不随 dialect
动态增加；方言的 op 名/属性名不靠关键字识别，而靠 `bare_identifier`/`hash_identifier`。

## 2. 主循环：`Lexer::lexToken`（`Lexer.cpp:80`）

结构是一个 `while (true)` 循环 + 对首字符的 `switch`：

1. **补全点检查**：`tokStart == codeCompleteLoc` 时直接产出 `code_complete`
   （`Lexer.cpp:87-88`），这是编辑器补全的唯一注入点。
2. **EOF 检查**（`:90-91`）。
3. 读一个字符后分派：

| 输入 | 处理 |
| --- | --- |
| 空格、`\t`、`\n`、`\r` | 跳过，继续循环（`:102-107`） |
| `\0` | 当作空白跳过（`:112-114`） |
| `:` `,` `(` `)` `[` `]` `<` `>` `=` `+` `*` `?` `\|` | 单字符标点 |
| `.` | `lexEllipsis`：必须是 `...`，否则报错（`:293-305`） |
| `-` | 后随 `>` 则 `arrow`，否则 `minus`（`:150-156`） |
| `/` | `//` 进入 `skipComment`，否则 `slash`（`:163-169`） |
| `@` | `lexAtIdentifier` |
| `#` | 若后随 `-}` 产出 `file_metadata_end`；否则与 `!`/`^`/`%` 一起走 `lexPrefixedIdentifier` |
| `"` | `lexString` |
| 数字 | `lexNumber` |
| 字母 | `lexBareIdentifierOrKeyword` |
| `_` | `lexBareIdentifierOrKeyword` |
| 其它 | `unexpected character` |

注意 `file_metadata_begin` 是 `{` 后跟 `-#` 两个字符的特判（`:127-131`），
`file_metadata_end` 是 `#` 后跟 `-}`（`:174-177`）。

## 3. 各类扫描

### 3.1 裸标识符与关键字（`Lexer.cpp:234`）

```ebnf
bare-id      ::= (letter | "_") (letter | digit | "_" | "$" | ".")*
integer-type ::= ("s" | "u")? "i" [1-9] [0-9]*
```

- 先按上述字符集吃满，再整体判定：
  - `i123` / `si8` / `ui16` → `inttype`（`:246-252`；实现只要求数字部分非空且全为数字，
    注释里的 `[1-9][0-9]*` 并未强制，例如 `i0` 也会被切成 `inttype`，宽度合法性留给类型解析）；
  - 否则查 `StringSwitch` 关键字表，命中为 `kw_*`，否则 `bare_identifier`。
- `.` 属于裸标识符字符，所以 `func.func`、`arith.addi` 各自是**一个** `bare_identifier`；
  只有整段拼写恰好等于关键字时才成为关键字（`func` 是 `kw_func`，`func.func` 不是）。
- `_` 也能作为首字符（主循环单独列出），但名称以数字开头不行。

### 3.2 数字（`Lexer.cpp:308`）

```ebnf
integer ::= "0x" hex_digit+ | digit+
float   ::= digit+ "." digit* (("e"|"E") ("-"|"+")? digit+)?
```

- 十六进制：`0x` 后必须紧跟十六进制位，否则只认 `0`（`:313-316`），
  所以 `0xi32` 会被切成 `0` + 裸标识符 `xi32`。
- 浮点必须以 `.` 出现为前提：`1.`、`1.5`、`1.0e-3` 是 float；
  **`1e5` 不是合法浮点**，会切成整数 `1` + 标识符 `e5`。
- 浮点指数部分只有在 `e/E` 后确实是数字（可带符号）时才吞入（`:335-343`）。
- 负号不是数字的一部分：`-` 是独立 `minus` token，由语法层处理（属性/affine 都如此）。

### 3.3 带前缀标识符（`Lexer.cpp:358`）

```ebnf
attribute-id ::= "#" suffix-id
ssa-id       ::= "%" suffix-id
block-id     ::= "^" suffix-id
type-id      ::= "!" suffix-id
suffix-id    ::= digit+ | (letter | id-punct) (letter | id-punct | digit)*
id-punct     ::= "$" | "." | "_" | "-"
```

- 若 suffix 以数字开头，则后续**只能是数字**：`%0`、`^12` 合法，`%0a` 报
  `invalid SSA name`。
- 否则首字符必须是字母或 `$ . _ -`，之后可混排字母/数字/标点。
- 这是 `%x#1`（结果号）能被正确切分的基础：`%x` 是 `percent_identifier`，
  `#1` 是另一个 `hash_identifier`（`#` 走同一函数的 fallthrough）。
- 若补全点落在标识符内部，会产出 `code_complete`，spelling 只保留到补全点
  （`:397-404`），供 LSP 计算候选。

### 3.4 `@` 标识符（`Lexer.cpp:205`）

```ebnf
symbol-ref-id ::= "@" (bare-id | string-literal)
```

两种形态：`@foo` 与 `@"foo bar"`（字符串可含空格/点等）。
后者在 `Token::getSymbolReference()` 里调用 `getStringValue()` 反转义。
非法起始（非字母、非 `_`、非 `"`）立即报
`@ identifier expected to start with letter or '_'`。

### 3.5 字符串（`Lexer.cpp:412`）

```ebnf
string ::= '"' ( [^"\n\v\f\r\\] | escape )* '"'
escape ::= "\\" ('"' | "\\" | "n" | "t" | hex_digit hex_digit)
```

- 允许 `\0` 字节原样出现在字符串里（`:428` 注释），但裸 `\n`、`\v`、`\f`、`\r`
  会报 `expected '"' in string literal`。
- 只支持 `\"`、`\\`、`\n`、`\t` 与 `\xx`（两位十六进制）五种转义；
  其它转义报 `unknown escape in string literal`。
- 没有 `\u`/Unicode 语义：字符串就是字节序列（`Token::getStringValue` 逐字节处理）。
- **词法不校验 UTF-8**，也不对标识符做 Unicode 处理（`isalpha` 为 C 库语义，实践中 ASCII）。

### 3.6 注释与空白

- `//` 到行尾（`\n` 或 `\r`），支持文件末尾无换行（`skipComment`，`:267-290`）。
- 注释只作为空白分隔，不产生 token；`emitWrongTokenError` 会利用注释位置
  优化诊断位置（见 07 章）。

## 4. 位置编码

`Lexer::getEncodedSourceLocation`（`Lexer.cpp:62`）把 `SMLoc` 变成
`FileLineColLoc`：

```cpp
auto [lineNo, column] = sourceMgr.getLineAndColumn(loc);
return FileLineColLoc::get(context, buffer->getBufferIdentifier(), lineNo, column);
```

- 每个 token 的 `getLoc()` 是 token 起始指针，因此 IR 里每个 op 的默认 location
  精确到它名字的首字符。
- 多 buffer 时，`Lexer` 构造函数会选择“被主 buffer 包含的最后一个 buffer”
  作为实际解析目标（`:40-56`），这是为了支持 `SourceMgr` 里附加的 include/内联缓冲。

## 5. Token 解码辅助

| 方法 | 用途 | 备注 |
| --- | --- | --- |
| `getUInt64IntegerValue` / `getUnsignedIntegerValue` | 十进制/十六进制整数 | `0x` 前缀自动按 16 进制 |
| `getIntTypeBitwidth` / `getIntTypeSignedness` | `i8`/`si8`/`ui8` | 无符号语义时返回 `nullopt` |
| `getStringValue` | 去引号并反转义 | 也支持 `@"..."` 与补全 token |
| `getHexStringValue` | `dense_resource` 等的二进制 blob | 要求 `0x` 前缀且长度为偶数 |
| `getSymbolReference` | `@name` → `name` | 字符串形式会反转义 |
| `getHashIdentifierNumber` | `#123` → `123` | `%x#1` 的结果号 |

## 6. 示例：一段文本的 token 流

```mlir
func.func @f(%arg0: i32) -> i32 {
  %0 = arith.addi %arg0, %arg0 : i32
  return %0 : i32
}
```

| 文本 | Token::Kind | 说明 |
| --- | --- | --- |
| `func.func` | `bare_identifier` | 含 `.`，不是关键字 |
| `@f` | `at_identifier` | |
| `(` `)` `{` `}` `:` `,` `=` | 对应标点 | |
| `%arg0` `%0` | `percent_identifier` | |
| `i32` | `inttype` | |
| `->` | `arrow` | |
| `arith.addi` | `bare_identifier` | |
| `return` | `bare_identifier` | 不是关键字（不在 TokenKinds.def 中） |
| `//` 注释 | 无 token | 直接跳过 |

## 7. 词法层易错点

1. `%0` 合法、`%0a` 非法；名称以数字开头时后续不能有字母。
2. `%x#1` 的 `#1` 不是名字的一部分，而是独立的 `hash_identifier`（结果号）。
3. `0x` 后无十六进制位时只认 `0`：`0xi32` = `0` + `xi32`。
4. `1e5` 不是浮点；必须写成 `1.0e5` 或 `1.e5`。
5. 关键字匹配整段拼写：`func` 是关键字，`func.func` 不是；
   `memref` 只有在裸写时才是 `kw_memref`。
6. `{-#` / `#-}` 是 file metadata 的专用 token，普通 `{`/`#` 不受影响。
7. 字符串无 Unicode 语义，非 ASCII 字节按原样保留但不会参与标识符词法。
8. lexer 不做任何语法校验（如 `<<<` 会得到三个 `less`），语法层负责报错。

下一章：[03-parser-core.md](03-parser-core.md)。
