# 第 21 章：C++ Triton 前端——从 Python AST 开始

## 目标

这一章不生成 TTIR，也不碰 GPUMin 后端。它只做一件基础工作：

> 把 Triton 的 Python kernel 源码变成 C++ frontend 能消费的 AST，并识别
> `@triton.jit`、普通参数和 `tl.constexpr` 参数。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch21.sh <llvm-bin-dir>
```

## 为什么用 C++ 写前端

Triton 的“Python 子集”不是一种新的词法/语法，而是 Python AST 上的一套
**语义**：

- `@triton.jit` 函数会被内联，不支持递归；
- `tl.constexpr` 参数在编译期求值；
- 标量和 tensor 有隐式的 `tt.splat`/broadcast；
- `tl.load`/`tl.store` 的 `mask`/`other` 有固定语义；
- `tl.sum`/`tl.max` 是 tensor reduction，不是 Python 的 `sum`。

这些语义全部由 C++ 实现。词法和语法分析继续用 CPython：

```text
kernel.py
  └─ CPython tokenize + ast.parse       （和 Triton 相同）
       └─ AST JSON
            └─ gpumin-triton（C++）
                 ├─ PyAst：AST JSON 读取
                 ├─ Frontend：识别 kernel/参数/constexpr
                 └─ TTIR-min（第 22 章开始填充）
```

Triton 自己也用 CPython 的 `tokenize` + `ast.parse`，前端从
`ast.NodeVisitor` 开始；`python/triton/compiler/code_generator.py` 的
`CodeGenerator` 是它的语义层。我们复用的是 parser，不是 Triton 的 C++
库。

## AST JSON 桥

C++ 工具不会链接 Python。它执行：

```text
python3 -c <ast-dumper> kernel.py
```

`ast-dumper` 递归地把 `ast.AST` 的 `_fields` 和 `lineno/col_offset`
序列化成 JSON。C++ 用 `llvm::json` 读入，之后不再需要 Python。

这样做有三个好处：

1. C++ 侧可以在没有 Python 开发头文件的环境里构建；
2. AST 是普通 JSON，测试可以固定成 fixture；
3. `--dump-ast-json` 和 `--ast-json` 让“解析”和“语义”可以分离调试。

## 识别规则

`Frontend::parseKernels()` 只认满足以下条件的函数：

- AST 节点是 `FunctionDef`；
- `decorator_list` 里存在 `@triton.jit`（`Attribute(attr="jit")`），也接受
  裸 `@jit` 作为教学简化；
- 参数来自 `posonlyargs`、`args`、`kwonlyargs`，以及 `vararg`/`kwarg`；
- 参数的 `annotation` 是 `tl.constexpr` 时标记为 `constexpr`。

没有 kernel 的文件会返回非零，并给出文件名：

```text
gpumin-triton: no @triton.jit kernel found in 'plain.py'
```

## 工具用法

```sh
# 构建
ninja -C <build-dir> gpumin-triton

# 列出 kernel 和参数
gpumin-triton --dump-ast scripts/triton/add_i32.py

# 打印 CPython AST JSON
gpumin-triton --dump-ast-json scripts/triton/add_i32.py -o /tmp/add.ast.json

# 不启动 Python，直接读 AST JSON
gpumin-triton --ast-json /tmp/add.ast.json --dump-ast

# 第 21 章的 TTIR 壳（第 22 章开始填充）
gpumin-triton --emit-ttir scripts/triton/add_i32.py
```

示例输出：

```text
kernel add_kernel (line 12)
  arg x_ptr
  arg y_ptr
  arg out_ptr
  arg n
  arg BLOCK (constexpr)
```

## 实现文件

| 文件 | 作用 |
| --- | --- |
| `llvm/tools/gpumin-triton/gpumin-triton.cpp` | 驱动：`--dump-ast`/`--dump-ast-json`/`--emit-ttir` |
| `PyAst.{h,cpp}` | 执行 CPython、读取 AST JSON、错误处理 |
| `Frontend.{h,cpp}` | 识别 `@triton.jit`、参数、`tl.constexpr` |
| `TTIR.{h,cpp}` | 第 21 章的 module 壳 |
| `scripts/triton/add_i32.py` | 后续章节共用的 i32 vecadd 契约 |
| `scripts/check-ch21.sh` | 本章门禁 |

## 与 Triton 的关系

我们不做 `ttir -> ttgir -> Triton llir`。第 21 章只确认：

- TTIR 之前的语法边界是 CPython AST；
- `@triton.jit`/`tl.constexpr` 的识别和 Triton 一致；
- 后面的 TTIR、布局、LLVM IR 都是 C++ 自己的实现。

这样既保留了 Triton 的语义，又避免了链接 Triton 的 MLIR/LLVM fork。

## 退出条件

- `ninja -C <build-dir> gpumin-triton` 成功；
- `gpumin-triton --dump-ast scripts/triton/add_i32.py` 输出 `add_kernel`
  和 `BLOCK (constexpr)`；
- `--dump-ast-json` 能产出 AST JSON，`--ast-json` 能在不启动 Python 的
  情况下复现同一份签名；
- 没有 `@triton.jit` 的文件返回非零；
- `check-ch21.sh <llvm-bin-dir>` PASS。

下一章：第 22 章把 `Frontend` 从“识别签名”推进到“生成 TTIR-min 的标量
SSA”。
