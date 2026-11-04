# 两个 TableGen 崩溃的确认与最小化

环境：`llvm-project @ fd64c07f9ae1`，`llvm-tblgen 24.0.0git`，
RelWithDebInfo + assertions（`/home/ubuntu/llvm/builds/pr-lane`）。
复现：`TBLGEN=/path/to/llvm-tblgen ./run.sh`。

上游 issue：[#238797](https://github.com/llvm/llvm-project/issues/238797)（`!head`/`!tail`）、
[#238799](https://github.com/llvm/llvm-project/issues/238799)（multiclass `dump`）。

| 文件 | rc | 结论 |
| --- | ---: | --- |
| `head_empty_field.td` | 134 | `!head(字段)`，字段最终为 `[]` → assert `Record.cpp:875` |
| `head_empty_deferred.td` | 134 | 单表达式：`!head(!filter(...))` 在 final resolve 变空 → 同一 assert |
| `tail_empty_deferred.td` | 134 | `!tail` 同理 → assert `Record.cpp:882` |
| `dump_multiclass_defm_class.td` | 139 | multiclass 里 `dump` + `defm X : MC, C;` → SIGSEGV |
| `dump_multiclass_defm_class_anon.td` | 139 | 匿名 `defm : MC, C;` 同样崩溃 |
| `dump_multiclass_defm_class_only_dump.td` | 139 | dump 是唯一 body 条目也崩溃 |
| `dump_multiclass_foreach.td` | 139 | dump 在 multiclass 内的 foreach 里也崩溃 |
| `dump_multiclass_noclass_ok.td` | 0 | 对照组：没有普通 class 父类 → 正常打印 `note: hi` |
| `assert_multiclass_defm_class_ok.td` | 0 | 对照组：`assert` 条目被显式跳过 |
| `defvar_multiclass_defm_class_ok.td` | 0 | 对照组：`defvar` 不进入 `NewEntries` |

---

## 1. `!head` / `!tail` 空 list

### 触发条件（精确）

1. 操作数**在解析期不是**具体 `ListInit`（否则 `ParseOperation` 已报
   `empty list argument in unary operator`，`TGParser.cpp:1380-1383`）。
2. 在 **final resolve** 时它变成空 `ListInit`：字段、模板参数、
   `!filter/!foreach/!listremove` 的延迟结果都可以。
3. `UnOpInit::Fold` 的 `HEAD`/`TAIL` 分支只有 `assert(!LHSl->empty())`
   （`Record.cpp:875` / `:882`），没有诊断。

最小复现（已入库）：

```tablegen
class C<int N> { int h = !head(!filter(x, [1, 2], !gt(x, N))); }
def D : C<5>;
```

```
$ llvm-tblgen -print-records head_empty_deferred.td
llvm-tblgen: .../Record.cpp:875: ... Assertion `!LHSl->empty() && "Empty list in head"' failed.
rc=134
```

`head_empty_field.td` 是更直观的版本：

```tablegen
def D { list<int> e = []; int h = !head(e); }
```

### 为什么是 bug

* assertions 构建：SIGABRT + "PLEASE submit a bug report"。
* release（NDEBUG）：`assert` 被编译掉，随后 `getElement(0)` / `slice(1)`
  在空容器上越界，属于 UB（`ListInit::getElement`/`SmallVector::operator[]`
  的断言同样只在 debug 生效）。
* 解析期守卫只覆盖具体字面量，final resolve 路径漏掉了同样的检查。

### 建议修复

在 `UnOpInit::Fold` 的 HEAD/TAIL 分支，把 `assert` 换成可恢复诊断，例如：

```cpp
case HEAD:
  if (const auto *LHSl = dyn_cast<ListInit>(LHS)) {
    if (LHSl->empty())
      PrintFatalError(CurRec->getLoc(), "!head applied to an empty list");
    return LHSl->getElement(0);
  }
```

（`LOG2` 已经有同样的 `PrintFatalError` 先例，`Record.cpp:930` 附近。）
回归测试放 `llvm/test/TableGen/`，用 `not llvm-tblgen %s 2>&1 | FileCheck`。

---

## 2. multiclass 中的 `dump` + `defm ... , Class`

### 触发条件（精确）

1. `multiclass` body 里有 `dump` 条目（directly / foreach 内均可）。
   `dump` 是 multiclass body 的合法语句（白名单 `TGParser.cpp:4530-4537`）。
2. 用 `defm` 实例化，且**在逗号后跟一个普通 class 作为父类**：
   `defm X : MC, C;`（匿名 `defm : MC, C;` 同样）。
3. `ParseDefm` 的 `InheritFromClass` 分支对每个条目调用
   `AddParentClass(RecordsEntry&, ...)`（`TGParser.cpp:4640-4655`）；
   该函数只处理 `Rec` 与 `Assertion`，其余直接 `Entry.Loop->Entries`
   （`TGParser.cpp:385-401`，崩溃行 393）。`Dump` 的 `Loop` 为空 → SIGSEGV。

不触发：

* `defm X : MC;`（没有普通 class 父类，根本不走该分支）；
* `assert` 条目（被显式跳过，`TGParser.cpp:390-391`）；
* `defvar` 不进入 `NewEntries`。

最小复现（已入库）：

```tablegen
class C;
multiclass MC { dump "hi"; def D; }
defm X : MC, C;
```

```
$ llvm-tblgen -print-records dump_multiclass_defm_class.td
Stack dump:
0  libLLVMSupport ... PrintStackTrace
...
4  libLLVMTableGen ... AddParentClass(RecordsEntry&, ClassReference&) + 76
5  libLLVMTableGen ... ParseDefm() + 2415
rc=139
```

### 历史

* `Entry.Loop->Entries` 的递归来自 2018-06-21 `7d69e0f37d282`
  （允许 multiclass 内 foreach 依赖模板参数）。
* `Assertion` 的特判来自 2021-04-30 `985ab6e1fa575`；`Dump` 从未被加入。
* `dump` 在 multiclass body 中一直合法，所以这是长期存在的崩溃路径，
  只在 "defm + 普通 class 父类 + dump" 组合下可达。

### 建议修复

与 `Assertion` 同等对待即可（`Dump` 已在 `RecordsEntry` 中，且 dump 消息在
`resolve()` 阶段已按实例打印）：

```cpp
if (Entry.Assertion || Entry.Dump)
  return false;
```

更严格的做法是给 `dump` 也定义"继承 class 时是否重复打印"的语义并加测试；
但无论哪种，都不应解引用空的 `Loop`。

回归测试：在 `llvm/test/TableGen/` 增一个
`multiclass-dump-defm-class.td`，RUN 行同时检查退出码与 `note:` 文本。

---

## 说明

* 两个问题都在 `llvm/test/TableGen` 现有 479 个测试之外；全量测试仍然
  479/479 通过，所以现有的 CI 不会发现它们。
* `run.sh` 里的 `Segmentation fault (core dumped)` 是 bash 对子进程的信号提示，
  不是脚本错误；退出码以上表为准。
