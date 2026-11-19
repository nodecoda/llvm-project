# 第 2 章：前端产出 `tinytt`（TTIR）

本章把 `kernel.py` 变成第 1 章定义的 `tinytt` 模块。关键约束：**不引入私有
IR**。AST 里每个 Python 值直接变成 MLIR `Value`，每条语句直接变成 MLIR op。

## 1. 为什么只复用 `PyAst`

上一条管线的 `Frontend.cpp` 有 1710 行，每个表达式都在构造私有 `TTIR` 类
（`F->createOp(...)`）。要把它接到 MLIR，等于把 1710 行全部重写，还会留下
两层 IR。

tiny Triton 的切法：

| 层 | 复用 | 说明 |
| --- | --- | --- |
| 语法 | `PyAst`（149 行） | CPython `ast.parse` → JSON，语法边界交给 Python |
| 语义 | 新写 `EmitTTIR.cpp` | AST → `tinytt` op，没有中间表示 |

`kernels/vecadd.py` 的 AST 节点只用到
`Module / FunctionDef / arguments / arg / Assign / BinOp / Call / Attribute /
Name / Constant / Expr / Return`——本章只处理这一组。

## 2. 驱动

```sh
tiny-triton kernels/vecadd.py --emit ttir \
  --arg-type a_ptr=ptr --arg-type b_ptr=ptr --arg-type c_ptr=ptr
```

- `--emit ttir` 是本章唯一支持的阶段；`ttgir` / `llir` 会明确报错，
  避免“跳过阶段”悄悄成立。
- `--arg-type NAME=ptr|ptr<f32>|i32|f32|i1`：Triton 的实参类型来自 launch
  特化，独立编译器必须显式给。没有默认值——缺一个就报错并指出行号。
- `--kernel NAME` 选择 kernel（缺省取第一个带 `@triton.jit` 的函数）。

## 3. Python 到 `tinytt` 的映射

| Python | 生成的 IR |
| --- | --- |
| `tl.program_id(0)` | `tinytt.get_program_id <axis = 0> : i32` |
| `tl.arange(0, 1024)` | `tinytt.make_range <axis = 0> : tensor<1024xi32>` |
| 标量 `*` 张量 / 标量 `+` 张量 | `tinytt.splat` + `arith.muli/addi` |
| `ptr + offs`（标量 ptr） | `tinytt.splat` 把指针广播成 `tensor<1024x!tinytt.ptr<f32>>`，再 `tinytt.addptr` |
| `tl.load(p)` | `tinytt.load` |
| `tl.store(p, v)` | `tinytt.store` |
| `a + b`（f32 张量） | `arith.addf` |

广播规则写成了一条不变量：**标量与张量混算时，先 `tinytt.splat` 到张量类型，
元素类型必须相等**，否则报错。这条规则同时适用于数值和指针。

## 4. 产物

`kernels/vecadd.ttir` 是本仓库的 golden，由前端输出直接生成：

```mlir
%5 = tinytt.splat %arg0 : !tinytt.ptr<f32> -> tensor<1024x!tinytt.ptr<f32>>
%6 = tinytt.addptr %5, %4
     : tensor<1024x!tinytt.ptr<f32>>, tensor<1024xi32> -> tensor<1024x!tinytt.ptr<f32>>
%7 = tinytt.load %6 : tensor<1024x!tinytt.ptr<f32>>, tensor<1024xf32>
```

参数是**标量** `!tinytt.ptr<f32>`，与真实 Triton 一致：`tt.splat` 负责
把指针广播成指针张量。第 1 章手写 golden 用的是张量参数，本章已按真实
Triton 语义更新。

## 5. 明确不支持（报错而不是降级）

- `for` / `if` / `while`：数据相关的控制流不在 tiny 管线里；第 7b 章
  只展开 `tl.static_range` 这种编译期已知次数的循环；
- `tl.sum` / `tl.dot` / `tl.trans`：第 6、7 章；
- 只给 mask 不给 other 的 `tl.load`；
- 整数除法。

诊断都带 `line N:` 前缀，直接指向 Python 源码行。

## 6. 门禁

```sh
tiny-triton/scripts/check-t2.sh /home/ubuntu/llvm/builds/pr-ctpop/bin
```

四项：

1. `tiny-triton --emit ttir` 成功；
2. 产物能被 `tiny-opt --verify-each` 接受；
3. 产物 canonical 形式与 `kernels/vecadd.ttir` 逐行一致；
4. 数据相关的循环负例被拒绝，且诊断包含源码行号与
   `only unrolls loops with constant bounds`。

## 7. 退出条件

- `ninja -C <build> tiny-triton` 成功，`check-t2.sh` 4/4；
- `check-t1.sh` 仍 5/5（golden 更新后重新验证）；
- `gpumin-verify-all.sh` 仍 8/8、`triton-verify-all.sh` 0 failed；
- `git tag tiny-t02` 指向本章 commit。
