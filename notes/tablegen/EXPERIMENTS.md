# TableGen 实验索引（可复现证据）

所有结论来自 `llvm-tblgen 24.0.0git`（assertions 构建）对
`llvm-project @ fd64c07f9ae1` 的实测；生成时间 2026-11-04。

```bash
source /home/ubuntu/llvm/toolset/env.sh
notes/tablegen/experiments/run.sh          # 重新生成 out/
# 或指定二进制：TBLGEN=/path/to/llvm-tblgen notes/tablegen/experiments/run.sh
```

`out/` 中的文件都是运行产物，可以随时删掉重建。

---

## 1. 正向实验（能跑通、看结构）

| 文件 | 证明什么 | 关键输出 |
| --- | --- | --- |
| `01_core.td` | class/def/继承/let/multiclass/defm/foreach/defset/匿名记录/`!instances` | 32 defs、4 anon、defm 名字 `M0_a`/`W0_w_a`、`!instances<Base>()` 全量快照 |
| `02_types.td` | Init/RecTy 转换边界、bits 切片/拼接、string/list/dag 算子 | `bits<4>→bits<8>` 必须拼接；`!getdagopname` 是根名；`!repr(bits)` 格式 |
| `03_ops.td` | 62 个 `!` 算子中常用部分的语义 | `!div(-7,2)=-3`；`!srl(-8,1)=9223372036854775804`；`!sort` 保元素类型；`!interleave` 返回 string |
| `04_preproc.td` | `include`、`#ifdef/#ifndef/#define`、`-D/-E`、depfile | 4 种 `-D` 组合；`-E` 只做预处理；depfile 内容 |
| `05_let_and_instances.td` | let 优先级、隐式 `NAME`、模板参数默认值、`!instances` 快照 | `D.x=3`、`E.x=2`、`G.n="G"`、`LA.seen=[RA,RB]`、`LB.seen=[RA,RB,RC]` |
| `06_defset_nested.td` | defset 顺序/嵌套/foreach 累积 | `Outer=[O0,I1,I2,O3]`、`Inner=[I1,I2]` |
| `07_list_unification.td` | list 元素类型按左到右统一；`[{` 是 code block | `[1,{0,1}]` → `[1, 1]`；`[ {0,1},1]` → `[{ 0, 1 }, 1]` |

### 01_core 关键片段（`out/01_core.records.txt`）

```
def M0_a { int n = 7;  string s = "leaf"; list<int> l = [7]; }
def M2_a { int n = 42; ... }              // let n = 42 in defm M2
def W0_w_a { ... }                       // 嵌套 defm：W0 + _w + _a
def CastCheck {
  list<Base> insts = [D0, D1, ..., anonymous_3];   // 27 条，含 defset 内记录
}
```

### 02_types 关键片段（`out/02_types.records.txt`）

```
bits<16> cat      = { 0,0,0,0,0,1,0,1, 0,0,0,0,0,1,0,1 };  // 左操作数在高位
int      fromBits = 5;        // bits<8> → int
bits<8>  widened  = {small, small};  // bits 不能隐式加宽
string   rootName = "root";   // !getdagopname((add:$root 1,2))
string   opname   = "add";    // !cast<string>(!getdagop(d))
```

### 03_ops 关键结果（`out/03_ops.records.txt`）

```
!add(1,2,3)=6        !sub(10,3)=7       !mul(2,3,4)=24
!div(7,2)=3          !div(-7,2)=-3      !logtwo(8)=3
!srl(-8,1)=9223372036854775804          !sra(-8,1)=-4
!range(1,7,2)=[1,3,5] !range(0,4,-1)=[] !sort(x,[3,1,2,1],x)=[1,1,2,3]
!listremove([3,1,2,1],[1])=[3,2]        !interleave(["a","b","c"],"-")="a-b-c"
!substr("abcdef",2)="cdef"              !find("banana","na",3)=4
!cast<string>(42)="42"                  !cast<bits<8>>(5)={...,1,0,1}
!instances<R>()=[RA,RB]                 !instances<R>("R[AB]")=[RA,RB]
```

`!if` 惰性：

```
int zero = 0;
int l1 = !if(1, 7, !div(1, zero));      // 7（死分支的参数仍被解析但不 fold）
int l2 = !if(zero, !div(1, zero), 8);   // 8（resolve 期短路）
// 若写成 !div(1, 0)，解析期就会 fatal：见 errors/09
```

### 04_preproc 结果（`out/04_preproc.variants.txt`）

```
[]                          → FromInclude HasA NoB
[-DFEATURE_A]               → FromInclude HasA NoB
[-DFEATURE_A -DFEATURE_B]   → BAndA FromInclude HasA HasB
[-DFEATURE_B]               → BAndA FromInclude HasA HasB
```

depfile：`/tmp/pp.inc: 04_preproc.td 04_preproc_inc.td`。

### 05 / 06 关键结果

```
def D { int x = 3; }        // body let > top-level let
def E { int x = 2; }        // top-level let > class default
def G { string n = "G"; }   // implicit NAME
def H { int vb = 15; string vn = "T5"; }  // 默认参数可用前面的参数
def LA { list<R> seen = [RA, RB]; }
def LB { list<R> seen = [RA, RB, RC]; }   // 记录 finalize 顺序决定快照

def Q {
  list<string> outer = ["O0", "I1", "I2", "O3"];
  list<string> inner = ["I1", "I2"];
}
```

---

## 2. 负例矩阵（`out/errors.txt`）

退出码来自不经过管道的直接运行；`134` = SIGABRT，`139` = SIGSEGV。

| 文件 | rc | 首个诊断（截断） |
| --- | ---: | --- |
| 01_dup_def | 1 | `def already exists: A`（+ previous definition note） |
| 02_bad_type | 1 | `Field 'x' of type 'int' is incompatible with value '"s"'` |
| 03_unknown_var | 1 | `Variable not defined: 'noSuchVar'` |
| 04_let_unknown | 1 | `Value 'noSuchField' unknown!` |
| 05_missing_targ | 1 | `value not specified for template argument 'C:a'` |
| 06_defset_type | 1 | `adding record of incompatible type 'B' to defset` |
| **07_dup_field** | **0** | 无错误——同 body 重复字段静默覆盖为后值 |
| **08_head_empty** | **134** | `Assertion '!LHSl->empty() && "Empty list in head"' failed` |
| 09_div_zero | 1 | `Illegal operation: division by zero` |
| 10_log_zero | 1 | `logtwo is undefined on arguments less than or equal to 0` |
| 11_cast_missing | 1 | `Undefined reference to record: 'nope'` |
| 12_getdagarg_oob | 1 | `!getdagarg index 5 is out of range (dag has 2 arguments)` |
| 13_self_field | 1 | `Recursion / self-assignment forbidden` |
| 14_cycle | 1 | 先 `Variable not defined: 'b'`，再 `could not be fully resolved: a` |
| 15_override_targ | 1 | `Value 'a' unknown!`（模板参数不能用 let 覆盖） |
| 16_unknown_class | 1 | `Couldn't find class 'NoSuchClass'` |
| 17_define_value | 1 | `only comments are supported after #define NAME` |
| 18_unterminated | 1 | `end of line in string literal` + 第二个 token 错误 |
| 19_template_arg_type | 1 | warning `unused template argument: C:a` + 类型错误 |
| 20_let_append_int | 1 | `expected '=' in let expression`（`+=` 不存在） |
| 21_let_append_ok | 0 | `let append/prepend` 对 list/string/dag 生效 |
| 22_let_append_bad | 1 | `Cannot append to field 'count' of type 'int'` |
| 23_unset_final_ok | 0 | 字段留 `?` 合法 |
| 24_unset_targ_default | 1 | `value not specified for template argument`（默认值 `?` 不完整） |
| **25_multiclass_dump_crash** | **139** | `AddParentClass` 空指针，见下 |

### 25 号崩溃（上游 bug）

```tablegen
class C;
multiclass MC { dump "hi"; def D; }
defm X : MC, C;
```

```
#4 TGParser::AddParentClass(RecordsEntry&, ClassReference&)  TGParser.cpp:385-401
#5 TGParser::ParseDefm()                                     TGParser.cpp:4647
```

`AddParentClass(RecordsEntry&)` 只处理 `Rec` 与 `Assertion`，其余类型直接
`Entry.Loop->Entries`；`Dump` 条目的 `Loop` 为空。完整输出：
`out/crash-dump-multiclass.txt`。

---

## 3. 规模与性能（`out/x86_timing.txt`）

X86.td：1573 classes / 87983 defs / 39028 anonymous / 3.09M 字段值。

| 命令 | 总耗时 | 产物 |
| --- | ---: | ---: |
| `--null-backend` | 1.09s（parse 1.03s） | — |
| `-gen-instr-info` | 1.74s | 9.0 MB |
| `-gen-dag-isel` | 3.35s（parse 1.10 / sort 0.63 / convert 0.79 / optimize 0.16 / emit 0.65） | 31.1 MB |
| `-gen-register-info` | 1.11s | 主文件 + 4 个附加文件 |
| `-gen-global-isel` | 1.13s | 7.2 MB |

复现（在 worktree 根）：

```bash
TBLGEN=$LLVM_HOME/builds/pr-lane/bin/llvm-tblgen
INC="-I llvm/lib/Target/X86 -I $LLVM_HOME/builds/pr-lane/include -I llvm/include -I llvm/lib/Target"
$TBLGEN --null-backend --time-phases $INC llvm/lib/Target/X86/X86.td
$TBLGEN -gen-dag-isel --time-phases $INC llvm/lib/Target/X86/X86.td -o /tmp/x.inc
```

---

## 4. 自建后端（`-gen-study`）

源码：`llvm/utils/TableGen/StudyEmitter.cpp`（131 行，已加入
`llvm/utils/TableGen/CMakeLists.txt:66`）。输出样例（`out/01_core.study.txt`）：

```
classes: 4  defs: 32  anonymous defs: 4  globals: 2
values on defs (incl. inherited): 94
root classes: 2  classes with template args: 1

== field types (defs) ==
  33 int
  31 string
  27 list<int>
...
== classes by direct derived records ==
  27 Base
  2 Shadow2
```

X86 上跑同一后端：

```
$TBLGEN -gen-study $INC llvm/lib/Target/X86/X86.td | head -30
classes: 1573  defs: 87983  anonymous defs: 39028  globals: 52
values on defs (incl. inherited): 3091869
top: Sched 19066, EVEX 16119, EVEX_CD8 15673, AVX512 13864
```

构建成本：改一个 .cpp → `ninja llvm-tblgen`，2 条边、约 3.3 秒。

---

## 5. 新增实验的约定

1. 正向实验放 `experiments/NN_topic.td`，负例放 `experiments/errors/NN_topic.td`。
2. 每个文件顶部写清楚"它证明什么"与关键预期。
3. 负例**不要**用 `| head` 取退出码（会得到 SIGPIPE 141）；用
   `out=$(cmd 2>&1); rc=$?`。
4. 在 `run.sh` 的循环里加文件名，保证 `out/` 可重建。
5. 结论写进 `TABLENGEN-INTERNALS.md` 对应章节，并给 `out/` 路径。
