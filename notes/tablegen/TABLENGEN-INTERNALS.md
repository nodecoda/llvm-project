# TableGen 源码深潜

> 基线：`fd64c07f9ae1`（upstream/main，2026-11-04），llvm-tblgen 24.0.0git（assertions）。
> 所有"实测"结论都能用 `notes/tablegen/experiments/` 复现；文件行号以本分支为准。

---

## 0. 一句话模型

TableGen 是**一个可扩展的记录实例化语言 + 一组一次性的多态后端**：

```
.td 文本
  └─(1) TGLexer        : 词法 + 极小的预处理器 + include
  └─(2) TGParser       : 递归下降，直接构造 Init DAG，边解析边折叠
  └─(3) RecordKeeper   : class/def/global 的注册表与索引
  └─(4) final resolve  : VarInit/FieldInit 替换、匿名名落地、类型检查
  └─(5) backend run()  : 遍历记录，向 raw_ostream 打印任意文本（通常是 C++）
```

三条主线：

1. **两个命名空间**：class/def 存在 `Classes`/`Defs`；`defvar`/`defset`/`deftype`
   之类存在 `ExtraGlobals` 或类型别名表。`getGlobal()` 先查 def 再查 global。
2. **两阶段求值**：解析期做"能算的"常量折叠；凡是依赖字段、模板参数、循环变量的
   表达式都留成 `VarInit` 或算子节点，等记录 finalize 时再 resolve。
3. **两个扩展点**：语言层（写 `.td`，用 class/multiclass/`!` 算子建模）与
   后端层（`TableGen::Emitter` 注册一个 `run(raw_ostream&)`）。

---

## 1. 调用栈：一次 `llvm-tblgen` 到底做了什么

入口是 18 行的 shim：`llvm/utils/TableGen/llvm-tblgen.cpp:13-18` → `tblgen_main`
（`llvm/utils/TableGen/Basic/TableGen.cpp:72-78`）→ `TableGenMain`
（`llvm/lib/TableGen/Main.cpp:203`）。

`TableGenMain` 的骨架（带计时器）：

| 阶段 | 位置 | 说明 |
| --- | --- | --- |
| 读输入/注册 SourceMgr | `Main.cpp:204-216` | `MemoryBuffer::getFileOrSTDIN(..., IsText=true)`；`-I` 加入 include 目录 |
| `-E` 预处理 | `Main.cpp:218-233` | 只 lex，不解析 |
| `RecordKeeper` | `Main.cpp:235-236` | 一个进程一个（全局 `SrcMgr`，非线程安全） |
| parse | `Main.cpp:244-249` | `TGParser(...).ParseFile()` |
| 选择后端 | `Main.cpp:258-268` | `TableGen::Emitter::ApplyCallback`，未注册则回退 `MainFn` 或失败 |
| 写 depfile | `Main.cpp:270-277` | `-d`；格式 `out: in deps...`（实测） |
| 写输出 | `Main.cpp:279-291` | `--write-if-changed` 时内容不变则丢弃 |
| 错误汇总 | `Main.cpp:252-255, 296-298` | 任何 `PrintError`（带 location）都会让退出码为 1 |

`ParseFile`（`TGParser.cpp:4732`）只做：`Lex.Lex()` → 建全局 scope → `ParseObjectList`
→ 必须 EOF。所有"语义"都发生在构造与 finalize 里。

**记录构建顺序**（`TGParser::ParseObjectBody`，`TGParser.cpp:3928`）：

```
名字 → 父类（AddParentClass:346，逐个复制字段 + 解析模板参数 + resolve）
     → 顶层 let（LetStack）
     → 记录体（ParseBody:3797；体内 let 立即 SetValue）
     → addEntry → addDefOne:580
          → Rec->resolveReferences(NewName)   // final resolve，Record.cpp:2898
          → checkConcrete / 重名检查 / defset 类型检查
          → Records.addDef                    // TGParser.cpp:623
```

这个顺序解释了两个常见现象：

* `let` 在字段引用解析**之前**生效，所以 `let Y = 10 in ... !add(Y,1)` 用的是新值；
* 体内 `let` 晚于顶层 `let`，因此同名时**体内赢**（实测：`D.x = 3` 覆盖 `let x = 2`，见
  `experiments/05_let_and_instances.td` 与 `out/05_let_and_instances.records.txt`）。

---

## 2. 源码地图

核心只有约 1.5 万行：

| 文件 | 行数 | 角色 |
| --- | ---: | --- |
| `llvm/lib/TableGen/TGParser.cpp` | 4846 | 递归下降语法、名字/作用域、defm/foreach/defset、let 应用 |
| `llvm/lib/TableGen/Record.cpp` | 3357 | Init 求值/折叠、类型系统、resolve、RecordKeeper 索引 |
| `llvm/include/llvm/TableGen/Record.h` | 2506 | 数据模型：Init/RecTy/Record/RecordKeeper |
| `llvm/lib/TableGen/TGLexer.cpp` | 1100 | 词法、include、`#ifdef` 预处理器 |
| `llvm/lib/TableGen/SetTheory.cpp` | 332 | 后端侧有序集合引擎（`(add ...)`/`sequence`） |
| `llvm/lib/TableGen/Main.cpp` | 310 | 驱动、输出、depfile、计时 |
| `llvm/lib/TableGen/JSONBackend.cpp` | 182 | `-dump-json` |
| `llvm/lib/TableGen/DetailedRecordsBackend.cpp` | 234 | `-print-detailed-records` |
| `llvm/lib/TableGen/TableGenBackend.cpp` | 119 | `-gen-*` 注册表与输出头 |
| `llvm/lib/TableGen/TableGenBackendSkeleton.cpp` | 64 | 最小后端模板（`-gen-skeleton-entry`） |
| `llvm/utils/TableGen/*.cpp` | ~50 文件 | 真实后端（InstrInfo/RegisterInfo/DAGISel/GlobalISel/...） |
| `llvm/test/TableGen/*.td` | 403 文件 | **最权威的语义测试集**，比文档更准 |

阅读顺序（每步都配合实验）：

1. `llvm/lib/TableGen/TableGenBackendSkeleton.cpp` —— 先看最小后端长什么样。
2. `Main.cpp:203-299` —— 驱动契约。
3. `TGLexer.h:31-190` + `TGLexer.cpp:343-700` —— token 与 include/预处理。
4. `TGParser.cpp:4682-4747`（语句分发）→ `3928-4040`（body/defset/deftype）。
5. `TGParser.cpp:4565-4668`（defm）+ `684-697`（模板参数解析）。
6. `TGParser.cpp:4123-4249`（foreach/if）+ `462-577`（统一的 expansion）。
7. `Record.h:289-500`（Init 根）→ 各子类 → `Record.cpp:774-1000`（UnOp 折叠）。
8. `Record.cpp:1742-2060`（Tern/Fold/IsA/Exists/Instances 折叠）。
9. `Record.cpp:2303-2510`（VarInit/FieldInit/VarDefInit 实例化）。
10. `Record.cpp:2855-2903` + `3308-3330`（final resolve 与 resolver）。

---

## 3. 词法、include 与预处理器

### 3.1 token 与两个"陷阱"

`tgtok::TokKind` 是连续分区（`TGLexer.h:31-160`）：标点、`IntVal/BinaryIntVal`、
预处理器内部 token（`Ifdef..Define`，**不会**由 `Lex()` 返回）、关键字、
object-start 关键字、`!` 算子、字符串类 token。`isObjectStart`/`isBangOperator`
只是范围判断（`TGLexer.h:177-190`）。

* **代码块 `[{ ... }]`**：`LexBracket`（`TGLexer.cpp:604`）只做原始字节扫描，
  遇到第一个 `}]` 就结束；不识别嵌套、字符串或注释。所以 code block 里的
  `}]` 会提前截断。另一个直接后果：list 字面量若以 bit-list 开头，
  `[{0,1}, ...]` 会被当成**未闭合的 code block**，必须写成 `[ {0,1}, ...]`
  （实测 `experiments/07_list_unification.td`）。
* **注释**：`//` 到行尾；`/* */` **可以嵌套**（`TGLexer.cpp:509-540`），和 C 不同。
* **字符串转义**只有 `\\ \' \" \t \n`（`LexString:343`），`\x`/`\0` 非法；
  相邻字符串字面量由**解析器**拼接（`TGParser.cpp:2991-2997`）。
* **数字**：`-3` 是单个 `IntVal`（`LexNumber:543`）。这影响旧式 range：
  `0-3` 被 lex 成两个整数，`ParseRangePiece` 再把后面的负数当上界（`TGParser.cpp:1055-1063`）。
  `0b101` 生成 `BinaryIntVal`，宽度=位数，解析成 `BitsInit`。
* **标识符可以以数字开头**（`isValidIDChar`，`TGLexer.cpp:40`），是为了 `foo#8i`
  这类 paste；`def 8` 仍然是数字。

### 3.2 `include` 不是预处理器指令

`include "file"` 在 lexer 里直接被吞掉（`LexInclude:469`）：读文件名、`SrcMgr.AddIncludeFile`、
把 `CurBuf/CurPtr` 切到被包含文件、压入新的预处理栈帧。解析器**永远看不到**文件边界。
`#include` 不存在——实测会报 `Unexpected token at top level`。

依赖收集在 lexer（`TGLexer::Dependencies`），`-d` 输出的就是它（实测：
`/tmp/pp.inc: 04_preproc.td 04_preproc_inc.td`）。

### 3.3 预处理器只有 5 个指令

`PreprocessorDirs`（`TGLexer.cpp:49`）：`#ifdef #ifndef #else #endif #define`。

* 条件只在**行首**（`#` 是行/文件第一个非空白字符）生效；否则 `#` 是 paste 算子。
* `#define M` **只登记名字，没有宏替换**；`#define M 1` 直接报错
  （`only comments are supported after #define NAME`，实测 `errors/17`）。
* 每个 include 有独立的控制栈，条件块不能跨文件（`prepExitInclude:705`）。
* `-D` 在命令行预定义；`-E` 只输出预处理后的文本（实测见 `out/04_preproc.E.txt`）。
* 重复 `#define` 只 warning。

---

## 4. 语法与对象模型

### 4.1 顶层语句

`ParseObject`（`TGParser.cpp:4682`）按 object-start token 分发：
`assert class def defm defset deftype defvar dump foreach if let multiclass`。

* `class` 可以有模板参数、父类列表、body；`class Foo;` 是受限前向声明
  （只有"没有字段/父类/模板参数"的 class 才能被后续定义复用，`ParseClass:4314-4358`）。
* `def` 名字在 `ParseNameMode` 下解析，允许 `#` paste；匿名 `def :`/`def {` 用
  `anonymous_N`（final resolve 时才落地，`Record.cpp:624-635`）。
* `multiclass` 是**解析期模板**，不是 RecordKeeper 里的记录（`TGParser.h:96-104`）。
* `defset`/`defvar` 进入 `ExtraGlobals`；`deftype` 只做类型别名。

### 4.2 defm 与名字改写

`multiclass` 里的成员用隐式模板参数 `NAME` 限定。`QualifyName`
（`TGParser.cpp:121-131`）生成：

* class：`C:a`；multiclass：`MC::NAME`。

`ParseDefm`（`4565`）把 defm 名绑定到 `MC::NAME`；`ParseObjectName`（`733`）
会给 multiclass 内的名字加上该前缀。**没有分隔符**，纯拼接：

```
multiclass Leaf { def _a; def _b; }
defm M0 : Leaf<7>;        → M0_a, M0_b
multiclass Wrapper { defm _w : Leaf<...>; def _p; }
defm W0 : Wrapper<1>;     → W0_w_a, W0_w_b, W0_p
```

实测见 `out/01_core.records.txt`。`defm` 还可以接普通 class 作父类
（`defm X : MC, Base;`），父类字段在展开后逐条 `AddParentClass`。

### 4.3 `let`：三个应用点与 append/prepend

* 体内 `let`：`ParseBodyItem` 立即 `SetValue`（`TGParser.cpp:3797-3853`）。
* 顶层 `let`：存 `LetStack`，在 `ParseObjectBody`（父类之后、body 之前）、
  `ParseDefm`（每个展开项）以及 `ApplyLetStack(RecordsEntry&)` 递归应用。
* 内层覆盖外层；body 覆盖顶层 let；**顶层 let 在解析 RHS 时看不到目标记录的字段**
  （`CurRec=nullptr`，`4393`）。
* 模板参数不能被 `let` 覆盖：实测 `def D : C<1> { let a = 2; }` 报
  `Value 'a' unknown!`（`errors/15`）。
* `let append/prepend` 是上下文关键字（`3752-3795`）：list→`!listconcat`、
  string/code→`!strconcat`、dag→`!con`；对 int 用会报
  `Cannot append to field 'count' of type 'int'`（实测 `errors/22`）。
  `+=` **不是** TableGen 语法（`errors/20`）。

### 4.4 作用域与重名

* `TGVarScope` 链：局部 `defvar` → 记录字段 → 模板参数 → 外层（`getVar:142-210`）。
  模板参数查找会把名字限定为 `C:arg` 并标记"已用"以抑制未使用 warning。
* 没有普通的前向引用：`ParseIDValue` 只查局部/已注册 global，否则
  `Variable not defined`（`1188-1216`）。唯一例外是具体 `def` 的自引用，
  被延迟成 `!cast(SelfType, NAME)`（`1207-1211`）。
* 重复 def 在 **finalize** 时才报 `def already exists`（`addDefOne:582-590`）；
  匿名记录撞名会被**静默改名**为新 `anonymous_N`。
* class/def 与 global 名字空间不同：`class Foo` 和 `defvar Foo` 可以共存，
  `ParseIDValue` 优先返回 def/global。

### 4.5 defset：有序列表，不是集合代数

`ParseDefset`（`3997`）要求 `list<T>` 类型，把 body 里 finalize 的每条 def
按**声明顺序**，类型检查（is-a）后追加；嵌套 defset 会同时被外层和内层收集。
实测：

```
defset list<A> Outer = {
  def O0 : A<0>;
  defset list<A> Inner = { foreach i = 1...2 in def I#i : A<i>; }
  def O3 : A<3>;
}
Outer = [O0, I1, I2, O3]   Inner = [I1, I2]
```

`SetTheory`（`lib/TableGen/SetTheory.cpp`）**不在前端**：它是有序 `SmallSetVector`
引擎，被后端用来解释 `(add ...)`/`(sub ...)`/`(sequence ...)` 等
（`CodeGenRegisters.cpp:618` 的 TupleExpander、`Target.td` 的 memberList）。
把两者混为一谈是常见误解。

---

## 5. 值系统：Init 与 RecTy

### 5.1 Init 层级（`Record.h`）

| 类 | 语义 | 类型 |
| --- | --- | --- |
| `BitInit` | 单个 bit，按 keeper 单例 | `bit` |
| `BitsInit` | 固定宽度位向量，内部 **LSB-first** | `bits<N>` |
| `IntInit` | int64 | `int` |
| `StringInit` | 不可变、interning；string/code 两个池 | `string` |
| `ListInit` | 元素 + 元素类型 | `list<T>` |
| `UnsetInit` | `?`；**没有 RecTy**，可进任意字段 | — |
| `DefInit` | 指向 Record；类型是 `D->getType()` | `RecordRecTy{direct supers}` |
| `DagInit` | 算子 Init + 可选的算子名 + 参数/参数名数组 | `dag` |
| `VarInit` / `FieldInit` / `VarBitInit` | 未解析引用 `X` / `X.Y` / `X{i}` | 声明类型/推导 |
| `VarDefInit` | `Class<args>` 的匿名实例化 | 类类型 |
| `UnOpInit` / `BinOpInit` / `TernOpInit` | 62 个 `!` 算子（见 §6） | 由 parser 指定 |
| `CondOpInit` / `FoldOpInit` / `IsAOpInit` / `ExistsOpInit` / `InstancesOpInit` | 专用算子 | 各自固定 |

注意：文档里常提到的 `IfInit`/`StringConcatInit`/`GetDagArgInit` 等**不是类**，
它们只是 `UnOp/BinOp/TernOp` 的 opcode（`Record.h:855-938, 983-995`）。
`MultiClass` 也不在 Record 模型里，是 `TGParser.h` 的模板容器。

### 5.2 类型关系：可转换 ≠ 子类型

`RecTy`（`Record.cpp:149-307`）两套关系：

* `typeIsA`（子类型）：默认指针相等；`ListRecTy` 逐元素、`RecordRecTy` 按父类集合。
* `typeIsConvertibleTo`：宽松。`bit ↔ int`、`int → bits<N>`（类型层不检查宽度）、
  `bits<N> → int`、`list<T>` 逐元素、string 只接受 string。
* `RecordRecTy` 的 `get()` 会把类集合排序并做成规范类型；**无关类求交得到 `{}`
  （AnyRecord），而不是错误**（`Record.cpp:236-237, 309-324`）。
* `resolveTypes` 的顺序：相同→公共父类→T2 可转→T1 可转→list 统一（`326-353`），
  因此 list 字面量的类型推断**对顺序敏感**。实测：

  ```
  list<int> a = [1, {0,1}];    // 先 int：bits<2> 转成 int，打印 [1, 1]
  list<int> b = [ {0,1}, 1];   // 先 bits<2>：内部元素类型是 bits<2>，打印 [{ 0, 1 }, 1]
  ```

  两者都能赋给 `list<int>`（元素可转换），但内部表示/打印不同，后端读
  `ListInit::getElementType()` 时会看到差异。

实测边界：

```
bits<8> b = bits<4>{...};   // 错误：bits 不能隐式加宽
bits<8> b = {small, small}; // 唯一加宽方式：位拼接
int i = b8;                 // OK：bits → int
bits<8> b8 = 42;            // OK：int → bits<8>（越界在 fold 时检查）
```

### 5.3 unset 的真相

`UnsetInit` 是 **concrete（`isConcrete()==true`）但 incomplete**
（`Record.h:498-500`）。后果：

* 字段最后留 `?` 是合法的（实测 `errors/23` 退出 0）；`checkConcrete` 不会拦。
* 模板参数默认值必须是 complete 的：`class C<int a = ?>` + `def D : C;` 报
  `value not specified for template argument`（实测 `errors/24`）。
* `!initialized(<字段>)` 在字段为 `?` 时**无法折叠**，报
  `could not be fully resolved`；只有字面量 `?` 或已初始化值能折叠（实测 §8.3）。
  根因：`VarInit::resolveReferences` 查不到 unset 字段时返回 `this`（`Record.cpp:2303`）。

### 5.4 bit/bits 的方向

`BitsInit` 的 index 0 是**最低位**（`Record.cpp:469-473`）；文本 `{a, b}` 与
`getAsString()` 都是 **MSB-first**，所以 parser 会先反转再构造
（`TGParser.cpp:3060-3092`）。JSON 的 bits 数组直接用 `getBit(0..N-1)`，
也就是 **LSB-first**（`JSONBackend.cpp:48-52`）——这是读写 JSON 时最容易踩的坑。

### 5.5 `true/false` 是 int

parser 把 `true`/`false` 映射成 `IntInit`（`TGParser.cpp:2966-2972`）；
`!eq`/`!match` 生成 `BitInit`，而 `!not`/`!isa`/`!exists` 生成 `IntInit`。
`getValueAsBit` 要求**恰好** `BitInit`，所以 int 型 0/1 会失败（`Record.cpp:3073`）。

---

## 6. 求值：什么时候算、由谁算

### 6.1 三种 resolver

| Resolver | 用途 | 关键行为 |
| --- | --- | --- |
| `MapResolver` | 模板参数、defm、foreach 替换 | 替换时临时擦除自身映射以防递归；期间不缓存 |
| `ShadowResolver` | `!foreach/!filter/!sort/!foldl` | 遮蔽循环变量，避免捕获外层同名 |
| `RecordResolver` | 最终解析字段 | 维护名字栈防环；**跳过 unset 字段**；`isFinal` 时才把匿名名具体化 |

final 开关是 `Resolver::setFinal`（`Record.h:2314-2322`）。`addDefOne` 调用
`Record::resolveReferences(NewName)`（`Record.cpp:2898`）严格 final；multiclass/loop
内部的拷贝用非 final 的 `MapResolver`，所以还能看到未解析占位。

### 6.2 解析期折叠 vs 最终解析（最重要的实践区别）

`ParseOperation` 每构造一个算子就 `->Fold(CurRec)`（例如 `TGParser.cpp:1419-1420`）。
折叠成功的立即变成常量；参数里含 `VarInit` 的则保留。

由此产生三条经验：

1. **死分支也会被解析期求值**。`!if(1, 7, !div(1, 0))` 会报 `division by zero`：
   `!div` 在参数解析时已经 fold 了；`!if` 的惰性只体现在 `resolveReferences`
   （`Record.cpp:1948-1962`，条件 resolve 成功后只 resolve 选中的分支）。
   要观察惰性，用变量条件：`!if(zero, !div(1, zero), 8)` → 8（实测 `03_ops`）。
2. **`!instances<T>()` 是"快照"**。它按记录 finalize 的顺序求值，只能看到当时
   已注册的 defs。实测：

   ```
   class R; def RA : R; def RB : R;
   class L { list<R> seen = !instances<R>(); bit hasRB = !exists<R>("RB"); }
   def LA : L;   // seen = [RA, RB]
   def RC : R;
   def LB : L;   // seen = [RA, RB, RC]
   ```
3. **绝对不要依赖未解析字段做类型/初始化判断**。见 §5.3 的 `!initialized`。

### 6.3 62 个 `!` 算子的实测语义（节选）

完整列表在 `TGLexer.cpp:630-706`；`ProgRef.md` 的附录是权威。实测要点：

| 算子 | 结论 |
| --- | --- |
| `!add/!mul` | 变参；`!sub/!div` 恰好 2 个参数 |
| `!div` | 向零截断：`!div(-7,2) = -3` |
| `!srl` / `!sra` | 逻辑/算术右移；`!srl(-8,1) = 9223372036854775804` |
| `!logtwo` | floor(log2)；0 或负数报错 |
| `!if` | 解析期参数仍求值；resolve 期短路 |
| `!switch` | 解析期降级为 `!eq` 链 + `!cond`（`2826-2947`） |
| `!sort(var, list, key)` | 返回**原元素类型**的 list（不是 key 的类型） |
| `!interleave(list, sep)` | 返回 **string**，不是 list |
| `!head/!tail` | 空 list 在 assertions 构建里直接 assert 崩溃（`08_head_empty`） |
| `!getdagop` | 默认静态类型是 `RecordRecTy{}`；必须 `!getdagop<Class>(d)` 才能赋给具体类型 |
| `!getdagopname` | 返回 **dag 根的 `$name`**（`(op:$root ...)` 的 `root`），不是算子名！ |
| `!getdagname` | 返回参数名；未命名返回 `?`（打印为 `?`） |
| `!cast<int>("42")` | **不支持**（没有 string→int 转换）；`!cast<string>(42)` 可以 |
| `!instances<T>()` | 必须写 `()`；只接受单个类名（多类 `{A,B}` 得到空列表） |
| `!exists<T>("name")` | 同上，返回 int |
| `!foldl/!foreach/!filter` | 循环变量用 `ShadowResolver`；`!filter` 保留谓词为真的元素 |
| `!repr` | 调试用、格式不保证稳定；record 会打印完整 body（多行） |

`!getdagop` 的坑有官方测试背书：`llvm/test/TableGen/getsetdagop.td` 的 ERROR1
明确写出 "static type of 'any record at all, with no required superclasses'".
实测：

```
def add;
dag d = (add:$root 1, 2:$a);
string opname = !cast<string>(!getdagop(d));  // "add"
string root   = !getdagopname(d);             // "root"
```

### 6.4 SetTheory（后端侧）

`SetTheory`（`SetTheory.cpp:257-268`）注册的算子：`add sub and shl trunc rotl
rotr decimate interleave sequence`。语义是**有序去重**（`SmallSetVector`）：

* `add` 并集、`sub` 差集、`and` 交集；
* `shl` 丢弃前 N、`trunc` 保留前 N、`rotl/rotr` 旋转、`decimate` 步进；
* `sequence "R%u", 0, 15[, stride]` 按格式串生成。

两个源码级风险：`sequence` 的 stride 为 0 时循环不减（`From += 0`，`SetTheory.cpp:180-236`）；
递归展开时先插入空缓存再展开，所以递归引用得到空集合（`308-332`）。

---

## 7. 后端：从 RecordKeeper 到 C++

### 7.1 注册表（没有大 switch）

`-gen-*` 是 `cl::opt<TableGen::Emitter::FnT>` 的字面选项，由静态对象注册：

```cpp
static TableGen::Emitter::OptClass<CTagsEmitter>
    X("gen-ctags", "Generate ctags-compatible index");   // CTagsEmitter.cpp:92
```

* `Opt` 构造时把回调塞进 `ManagedStatic<cl::opt<FnT>>`（`TableGenBackend.cpp:46-59`）。
* `ByDefault=true` 的那个就是 `print-records`（`Basic/TableGen.cpp:58-70`）。
* `ApplyCallback`（`TableGenBackend.cpp:63-79`）按回调类型调用单文件或多文件生成器。
* `OptClass<E>` 要求 `E(const RecordKeeper&)` + `run(raw_ostream&)`；
  `MultiFileOptClass<E>` 要求 `run(StringRef FilenamePrefix)` 返回
  `TableGenOutputFiles{MainFile, AdditionalFiles}`（`TableGenBackend.h:59-81`）。

历史上 llvm-tblgen 用 enum + 大 switch；现在只有 **clang** 的 tblgen 还这样
（`clang/utils/TableGen/TableGen.cpp:25,134,383`）。`utils/TableGen/TableGenBackends.h`
已经不再是注册表，只剩两个遗留声明。

### 7.2 输出与构建集成

* `emitSourceFileHeader`（`TableGenBackend.h:91-94`）打印 `TableGen'erated file` 头；
  `From:` 来自 `RecordKeeper::getInputFilename()`。注意它默认参数是
  **临时** `RecordKeeper()`，后端若想输出文件名必须把 `Records` 传进去。
* 多文件后端：主文件写到 `-o`，其余按 `stem + suffix` 命名
  （`Main.cpp:282-288`）。`RegisterInfoEmitter` 产出 `Enums/MCDesc/Header/TargetDesc`
  四个附加文件，名字必须与 CMake 的 `EXTRA_OUTPUTS` 同步（`RegisterInfoEmitter.cpp:2150`）。
* CMake：`add_tablegen` 定义 `${project}_TABLEGEN_EXE` 并导出 `LLVM_TABLEGEN`
  （`llvm/cmake/modules/TableGen.cmake:207-242`）；`tablegen_internal`（`:16`）
  拼 `-I`、depfile、`LLVM_TABLEGEN_FLAGS`。头文件生成目录设
  `LLVM_TABLEGEN_PROJECT LLVM_HEADERS`，用 `llvm-min-tblgen`（只含 Basic 后端）
  以避免 "headers 等全量 tblgen" 的循环依赖。

### 7.3 新增一个后端的完整配方（本分支已实践）

1. 复制 `TableGenBackendSkeleton.cpp` 的类形状：`(const RecordKeeper&)` + `run(OS)`。
2. 在同一个 TU 里注册：`static TableGen::Emitter::OptClass<FooEmitter> X("gen-foo", "...");`
3. 把文件加进 `llvm/utils/TableGen/CMakeLists.txt` 的 `add_tablegen(llvm-tblgen ...)`。
4. 测试：`llvm/test/TableGen/Foo.td`，RUN 行用 `llvm-tblgen -gen-foo %s | FileCheck %s`。
5. 重新构建只需该 .o + 链接（实测 3.3 秒，2 条边）。

本分支的 `llvm/utils/TableGen/StudyEmitter.cpp`（131 行）是实例：它输出
class 数/def 数/匿名数/global 数、字段类型直方图、直接派生 Top10、
前 10 个 class 与前 10 个匿名 def。运行：

```bash
llvm-tblgen -gen-study notes/tablegen/experiments/01_core.td
llvm-tblgen -gen-study -I ... llvm/lib/Target/X86/X86.td | head -30
```

X86 实测输出（assertions 构建）：

```
classes: 1573  defs: 87983  anonymous defs: 39028  globals: 52
values on defs (incl. inherited): 3091869
root classes: 250  classes with template args: 1050
top direct-derived: Sched 19066, EVEX 16119, EVEX_CD8 15673, AVX512 13864, ...
```

这组数字本身就是"TableGen 在 LLVM 里规模有多大"的答案。

### 7.4 真实后端案例

* **InstrInfoEmitter**（`InstrInfoEmitter.cpp:922` `run`）：枚举 → 操作数信息去重
  → `MCInstrDesc` 行（`emitRecord:1295`）→ 隐式 uses/defs → 字符串表 → 初始化函数。
  每个 `Instruction` 记录的字段被压成 `MCID::*` 位与 `TSFlags`。
* **RegisterInfoEmitter**（`run:2123`）：多文件；`runEnums/runMCDesc/runTargetHeader/runTargetDesc`。
  子寄存器/超寄存器用差分序列 + `SequenceToOffsetTable`。
* **DAGISel**：`DAGISelEmitter::run:152` 四步——
  `PatternSortingPredicate` 排序（`84-124`）→ 每个 pattern 变体
  `ConvertPatternToMatcherList` → `OptimizeMatcher`（`DAGISelMatcherOpt.cpp:25` 收缩、
  `:353` 因式分解）→ `MatcherTableEmitter::EmitMatcherTable`（`:1602`）。
  产物是 `SelectionDAGISel::SelectCodeCommon` 解释的字节码表。
* **GlobalISel**：`GlobalISelEmitter::run:2481` 先收集 opcode/type/节点等价，
  再逐 pattern `runOnPattern:2160` 构建 `RuleMatcher`，按 score 稳定排序，
  最后 `buildMatchTable` + 生成 executor 代码。注意它复用
  `CodeGenDAGPatterns`，所以 `parse` 与 DAGISel 同源。

---

## 8. 性能：先量再猜

同一台机器、assertions 构建、X86.td（88k defs / 1.57k classes）：

| 命令 | 总耗时 | 关键阶段 | 产物 |
| --- | ---: | --- | ---: |
| `--null-backend` | 1.09s | parse 1.03s | — |
| `-gen-instr-info` | 1.74s | parse ~1.0s，其余分散 | 9.0 MB |
| `-gen-dag-isel` | 3.35s | parse 1.10；sort 0.63；convert 0.79；optimize 0.16；emit 0.65 | 31.1 MB |
| `-gen-register-info` | 1.11s | parse 1.02；print 类阶段 ms 级 | 5 个文件 |
| `-gen-global-isel` | 1.13s | 几乎全是 parse | 7.2 MB |

结论：

1. **parse 是固定成本**，X86 约 1 秒；后端耗时只有 DAGISel 这种"图上优化"才显著。
2. 想量后端：`--time-phases`；想量纯解析：`--null-backend --time-phases`。
3. 大改动后先跑 `-print-records`/`-gen-study` 看记录数是否爆炸（匿名记录、foreach 展开）。
4. `getAllDerivedDefinitions` 单类有缓存，**任何 `addDef` 都会清空**；
   多类重载每次都扫描所有 `DefsByDirectSuperClass` 分组并排序去重（`Record.cpp:3244-3273`）。
   在循环里反复调用它（尤其在 88k defs 上）是常见的自造热点。

---

## 9. 调试技术

| 手段 | 命令 | 用途/坑 |
| --- | --- | --- |
| 记录转储 | `-print-records` | 字段值全部 resolve 后打印；看不到未解析算子 |
| 详细转储 | `-print-detailed-records` | 带 source loc/field kind；globals 段**只列 ExtraGlobals**（`defvar`/`defset`），def 名不出现在 globals 段 |
| JSON | `-dump-json` | `!tablegen_json_version:1`；丢字段类型/code 标记；bits 是 LSB-first |
| 宏展开 | `-E` | 只看预处理结果 |
| 集合 | `-print-sets` | 用 `SetTheory` 展开 dag 集合（`llvm/test/TableGen/SetTheory.td`） |
| 阶段耗时 | `--time-phases` | 每阶段 user/system/wall；后端可自定义 timer |
| 最小后端 | `-gen-skeleton-entry` | 后端 API 的最小可运行样本 |
| 自建后端 | `-gen-study` | 本分支的 `StudyEmitter.cpp`，可直接改成你的探针 |

加探针的推荐姿势：不要往 parser 里塞 printf；先加一个只读后端，
用 `getClasses/getDefs/getAllDerivedDefinitions/getValue*` 把状态打出来。
parser 的中间状态（scope/LetStack/多阶段 resolve）才需要临时 `LLVM_DEBUG`。

---

## 10. 专家级 gotcha 清单（每条都有证据）

> 标记【实测】的可在 `out/` 或 `experiments/errors/` 复现；其余为源码/测试证据。

1. 【实测】同一 body 内重复字段**静默覆盖**，不报错：`int x = 1; int x = 2;` → x=2
   （`errors/07`；`AddValue:213` 对已存在字段只 `setValue`）。
2. 【实测】`!head(空 list)` 在 assertions 构建直接 assert 崩溃（`errors/08`，SIGABRT）。
   生产构建行为未定义；不要在 `.td` 里依赖空 list。
3. 【实测】`!initialized(字段)` 在字段为 `?` 时无法 resolve；字面量 `?` 才行（§5.3）。
4. 【实测】`!getdagopname` 返回 dag 根的 `$name`，不是算子名；未命名时是 unset。
   取算子名用 `!cast<string>(!getdagop(d))`（`getsetdagop.td` + 本分支探针）。
5. 【源码】`!getdagop` 默认静态类型是 `RecordRecTy{}`；不加 `<Class>` 赋值给具体类型会报
   static type 错误。字符串算子的 dag 无法用 `!getdagop` 取回（类型不匹配）。
6. 【实测】`!cast<int>("42")` 不支持；只有到 string / record / 可转换类型的 cast。
7. 【实测】`!if` 只在 resolve 期短路；字面量死分支的参数已经在解析期 fold
   （`!if(1, 7, !div(1,0))` 报 division by zero）。
8. 【实测】`!instances/!exists` 是 finalize 时的快照，记录声明顺序会改变结果（§6.2）。
9. 【实测】`!sort` 返回元素类型，不是 key 类型；`!interleave` 返回 string。
10. 【实测】`!instances<T>` 必须带 `()`；多类类型 `{A,B}` 不是类名，得到空列表。
11. 【实测】`defset` 可以嵌套，外层按声明顺序收集内层产生的记录；
    `foreach` 的每次迭代都会 append（`sem3`）。
12. 【源码】`defset`/`defvar` 与 class 同名合法；`getGlobal` 只看 defs + globals。
13. 【实测】没有普通前向引用；`def D { int a = b; int b = 1; }` 报
    `Variable not defined: 'b'`（`errors/14`）。自引用只对具体 def 开放。
14. 【实测】模板参数不能用 `let` 覆盖，报 `Value 'a' unknown!`（`errors/15`）。
15. 【实测】`#define` 没有值，`#define M 1` 报错；`#include` 不存在。
16. 【源码】`[{...}]` code block 遇到第一个 `}]` 截止，不识别嵌套/字符串/注释。
17. 【源码】`-3` 是单个 token，导致旧式 `0-3` range 的解析依赖"负数当上界"的规则。
18. 【源码】`/* */` 可以嵌套；C 程序员会误判注释结束位置。
19. 【源码】字段声明加 `field` 前缀会设置 `FK_NonconcreteOK`，跳过 concreteness 检查
    （`TGParser.cpp:3619-3627`，`checkConcrete:89-115`）。`field` 是给"故意保留未解析值"
    的 CodeEmitterGen 类场景留的逃生口。
20. 【实测/源码】`multiclass` 里放 `dump`，再用 `defm X : MC, SomeClass;` 会
    **段错误**：`AddParentClass(RecordsEntry&)`（`TGParser.cpp:385-401`）对
    Dump 条目无条件解引用 `Entry.Loop`。最小复现与 backtrace 见
    `out/crash-dump-multiclass.txt`。这是上游 bug，不是用法问题。
21. 【源码】`DetailedRecordsBackend` 用 `getGlobals()`（只含 ExtraGlobals），
    所以 `defvar` 可见、`def` 不可见；想看全部请用 `-print-records`。
22. 【源码】`getAllDerivedDefinitions` 只索引 defs，不返回 class；结果引用内部可变
    vector，后续 `addDef` 会使其失效。
23. 【源码】`SetTheory::sequence` stride=0 死循环；递归展开得到空集。
24. 【源码】JSON 后端对未处理的 concrete Init 会 assert（`JSONBackend.cpp:110-115`），
    且丢失字段类型与 code/string 区分。

---

## 11. 练习：把"读懂"变成"能改"

每个练习都要求给出 `-print-records`/后端输出作为验收证据。

1. **名字改写**：用 `multiclass` + 嵌套 `defm` 生成 `A_b_c`；预测 `NAME` 在每层
   的值，再用 `!repr(NAME)` 验证。
2. **let 优先级**：构造 4 层（base class / subclass / 顶层 let / body let），
   预测最终值并验证。
3. **类型边界**：写一个 `bits<8>` 与 `bits<4>` 的拼接/切片/转换矩阵，
   标出哪些是 static error、哪些是 fold-time error。
4. **list 推断**：用 `[1, {0,1}]` 与 `[{0,1}, 1]` 观察元素类型；
   解释 `resolveTypes` 的顺序敏感性。
5. **unset**：分别用字面量、字段、模板参数默认值测试 `!initialized`，
   解释为什么只有一种能折叠。
6. **dag**：用 `!getdagop/!getdagopname/!getdagname/!setdagop/!setdagname`
   实现"把 (add a, b) 改名为 (mul:$root a, b)"，并说明静态类型。
7. **求值时机**：构造一个 `!instances` 在 class 中的例子，使两个实例看到不同集合。
8. **defset vs SetTheory**：用 `defset` 收集记录，再用后端的 `SetTheory`
   （或 `-print-sets` 的测试）做并/交/差；说明为何前端不能做集合代数。
9. **错误定位**：从 `errors/*.td` 任选 5 条，写出对应的源码函数与行号。
10. **写后端**：给 `-gen-study` 增加"每个 class 的实例数（含间接派生）"与
    "字段类型树"，要求一次遍历完成（O(classes+defs)）。
11. **性能**：在 X86.td 上对比 `--null-backend`、`-gen-instr-info`、`-gen-dag-isel`
    的 `--time-phases`，解释 DAGISel 多出来的三个阶段。
12. **上游级修复**：修 §10.20 的 `AddParentClass` 空指针（在 `Dump` 分支返回 false），
    加 `llvm/test/TableGen/` 回归测试，跑 `llvm/test/TableGen` 全量。

---

## 12. 继续深入：按问题找入口

| 你想知道 | 先看 |
| --- | --- |
| 某个 `.td` 语法怎么解析 | `ParseObject:4682` 分发 → 对应 `ParseX` |
| 某个 `!` 算子怎么算 | `TGLexer.cpp:630`（名字）→ `ParseOperation`（类型）→ `Record.cpp:774+`（Fold） |
| 名字为什么变成这样 | `QualifyName:121`、`ParseObjectName:733`、`addDefOne:580` |
| 字段值为什么是 X | `ParseObjectBody:3928` → `Record::resolveReferences:2855` → `RecordResolver::resolve:3308` |
| 模板参数怎么替换 | `resolveArguments:627`、`resolveArgumentsOfMultiClass:684`、`VarDefInit::instantiate`（`Record.cpp:2368`） |
| 生成的 .inc 从哪来 | 在 `llvm/utils/TableGen/` 搜 `gen-<name>`；从 `run()` 入口读 |
| 为什么构建慢/重建多 | `TableGen.cmake:16-160`；`TableGenBackends.h:47-55` 的 FIXME |
| 语义到底该是什么 | `llvm/test/TableGen/*.td`（403 个文件）优先于 `docs/TableGen/` |

最后一条经验：**TableGen 的"文档"是三层**——`ProgRef.md`（语言）、
`BackGuide.md`（后端）、`llvm/test/TableGen`（可执行规范）。三者冲突时，
以测试和源码为准；本分支的 `EXPERIMENTS.md` 就是把这些冲突点固化成可复现实验。
