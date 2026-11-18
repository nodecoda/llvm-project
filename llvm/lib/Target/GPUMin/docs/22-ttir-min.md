# 第 22 章：TTIR-min——C++ 的 SSA 与标量语义

## 目标

第 21 章只把 Python AST 读进 C++。这一章开始真正“编译”：

> 用 C++ 类表示 TTIR 子集，把标量 kernel 的语句和表达式 lowering 成
> SSA，并打印出 Triton 的 `triton-opt` 能解析的文本。

本章不做 tensor、不做指针、不做 `tl.load`/`tl.store`。这些留给第 23 章；
先把最小的语义闭环做对：常量、变量、整数运算、比较、返回。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch22.sh <llvm-bin-dir>
# 可选：让门禁调用真实的 Triton 解析器
TRITON_OPT=/path/to/triton-opt llvm/lib/Target/GPUMin/scripts/check-ch22.sh <llvm-bin-dir>
```

## 为什么 IR 是 C++ 类，不是文本

很多教学项目直接拼 TTIR 字符串。这会把两个问题混在一起：

1. SSA 的构造是否合法（值是否有定义、类型是否一致）；
2. 文本是否恰好长得像 Triton。

本章把 **C++ 类作为单一事实来源**，printer 只是投影：

```text
Python AST
  └─ Frontend（Lowerer）
       └─ TTIR.h：Type / Value / Op / Function / Module
            └─ Function::print / Module::print
```

这样 gate 可以分别验证“语义对不对”和“文本像不像”；第 23 章加 tensor 时
也只需要扩展类和 printer，不需要重写字符串拼接逻辑。

## 类型系统

`TTIR.h` 的 `Type` 从第一天就定义了后续章节要用的种类：

| Kind | 文本 | 本章状态 |
| --- | --- | --- |
| `I1` | `i1` | 比较结果 |
| `I32` | `i32` | 唯一参与算术的标量类型 |
| `F32` | `f32` | 第 25 章 |
| `Ptr` | `!tt.ptr<...>` | 第 23 章 |
| `Tensor` | `tensor<...>` | 第 23 章 |

本章的规则很简单：

- 所有运行时参数都是 `i32`（Triton 里 `n: i32` 的语义）；
- 整数字面量是 `i32`；
- `arith.cmpi` 的结果是 `i1`；
- 二元算术要求两个操作数同为 `i32`，否则报错。

`tl.constexpr` 参数是编译期值，不出现在 `tt.func` 的运行时参数表里。第 23
章会把它们折叠成常量。

## SSA 与命名

参数按名字打印，不占用编号；编号从第一个“结果值”开始：

```mlir
tt.func public @scalar_add(%n: i32) -> i32 {
  %0 = arith.constant 1 : i32
  %1 = arith.addi %n, %0 : i32
  tt.return %1 : i32
}
```

这与 Triton 的 TTIR 形状一致。Triton 在函数参数和常量上还会带 `loc(...)`、
`tt.divisibility` 和 `attributes {noinline = false}`；本章不生成这些属性。
门禁的做法是：把两边都送进 `triton-opt --canonicalize`，属性与 SSA 重命名
被规范化后再 diff。

## lowering 规则

`Frontend::lowerKernels()` 为每个 `@triton.jit` 函数创建一个 `Function`，
再由 `Lowerer` 逐语句、逐表达式下降。

语句：

| Python AST | TTIR | 说明 |
| --- | --- | --- |
| `x = expr` | 环境绑定 | 目标必须是简单 `Name` |
| `return expr` | `tt.return %v : ty` | 无值时为 `tt.return` |
| `pass` | 无 | 允许空 kernel |
| 字符串表达式 | 无 | docstring 被跳过 |
| 其他 | 报错 | 带 `file:line:col` |

表达式：

| Python AST | TTIR |
| --- | --- |
| 整数常量 | `arith.constant` |
| `Name` | 环境查表 |
| `+ - * & | ^ <<` | `arith.addi/subi/muli/andi/ori/xori/shli` |
| `>>` | `arith.shrsi`（Python 有符号右移） |
| `== != < <= > >=` | `arith.cmpi` |
| 一元 `+` / `-` | 恒等 / `0 - x` |
| `Call` | 第 23 章（`tl.*`） |
| 其他 | 报错，例如 `ListComp` |

错误信息统一带源码位置：

```text
gpumin-triton: bad.py:6:9: unsupported expression 'ListComp'
```

## 与 Triton 对拍

`scripts/triton/scalar_add.py` 是本章样例：

```python
@triton.jit
def scalar_add(n):
    return n + 1
```

本工具输出：

```mlir
module {
  tt.func public @scalar_add(%n: i32) -> i32 {
    %0 = arith.constant 1 : i32
    %1 = arith.addi %n, %0 : i32
    tt.return %1 : i32
  }
}
```

`scripts/triton/scalar_add.ttir` 存的是 Triton 前端的 canonical 形式
（`%arg0`、`%c1_i32`）。门禁在 `TRITON_OPT` 可用时：

1. 用 `triton-opt` 解析我们的文本，确认语法合法；
2. 两边各跑一次 `triton-opt` canonicalize；
3. 抽出 `@scalar_add` 后 `diff`，必须为空。

`TRITON_OPT` 未设置时这两项显示 SKIP；本章自己的 lowering 检查仍然执行。

## 实现文件

| 文件 | 作用 |
| --- | --- |
| `llvm/tools/gpumin-triton/TTIR.{h,cpp}` | 类型、值、操作、函数、模块与 printer |
| `llvm/tools/gpumin-triton/Frontend.{h,cpp}` | `Lowerer`：语句/表达式下降与错误定位 |
| `llvm/tools/gpumin-triton/gpumin-triton.cpp` | `--emit-ttir` 改为输出真实 lowering |
| `scripts/triton/scalar_add.py` | 标量样例 |
| `scripts/triton/scalar_add.ttir` | Triton canonical golden |
| `scripts/check-ch22.sh` | 本章门禁 |

第 21 章门禁的第 5 项原本用 `add_i32.py` 检查“打印一个 module”。从本章
起 `add_i32.py` 会因为 `tl.program_id` 停在 chapter-23 边界，所以第 21 章
门禁改为使用临时标量 kernel；检查语义不变。

## 与 Triton 的差异

- 不生成 `loc(...)`、`tt.divisibility`、`attributes {noinline = false}`；
- 只覆盖标量 i32，不覆盖 `float`、tensor、指针；
- 不实现 Triton 的隐式 `tt.splat`/broadcast——那是第 23 章的类型规则；
- 不链接 Triton/MLIR，`triton-opt` 只在门禁里作为外部对拍工具。

## 下一章

第 23 章把类型系统推到 tensor 和指针：

- `tl.program_id` → `tt.get_program_id`；
- `tl.arange` → `tt.make_range`；
- 标量与 tensor 混用 → `tt.splat`；
- 指针 + 偏移 → `tt.addptr`；
- `tl.load`/`tl.store` + mask → `tt.load`/`tt.store`。

到那时，`scripts/triton/add_i32.py` 才能完整下降，并与 Triton 的 TTIR 对拍。
