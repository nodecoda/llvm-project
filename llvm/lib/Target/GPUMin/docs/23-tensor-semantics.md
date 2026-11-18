# 第 23 章：tensor、指针与 mask

## 目标

第 22 章只处理标量。这一章把前端推到 Triton kernel 的真实形状：

> `tl.program_id`、`tl.arange`、标量与 tensor 混用、指针加法、
> `tl.load`/`tl.store` 以及 mask，全部下降到 TTIR-min，并能被
> `triton-opt` 解析。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch23.sh <llvm-bin-dir>
# 可选：同时做 Triton 解析与 canonical 对拍
TRITON_OPT=/path/to/triton-opt \
  llvm/lib/Target/GPUMin/scripts/check-ch23.sh <llvm-bin-dir>
```

## 输入从哪来：launch specialization

Triton kernel 的参数类型和 `tl.constexpr` 值不写在源码里，而来自**启动时的
specialization**（实参是 tensor 还是标量、`BLOCK` 取多少）。本工具不启动
kernel，因此由命令行显式给出：

```sh
gpumin-triton --emit-ttir \
  --constexpr BLOCK=1024 \
  --arg-type x_ptr=ptr --arg-type y_ptr=ptr --arg-type out_ptr=ptr \
  scripts/triton/add_i32.py
```

- `--constexpr NAME=INT`：绑定 `tl.constexpr` 参数；未绑定时报错并提示修改方法；
- `--arg-type NAME=i32|ptr`：未给出的运行时参数默认 `i32`；
- `ptr` 表示 `!tt.ptr<i32>`（GPUMin 的地址空间与指针宽度在第 24 章接入）。

这是与 Triton 的唯一接口差异：Triton 把 specialization 放在 launcher 里，
我们把它放在命令行上，之后的 lowering 完全相同。

## 类型规则

本章只增加两种结构类型：

| 类型 | 文本 |
| --- | --- |
| 指针 | `!tt.ptr<i32>` |
| tensor | `tensor<Nxi32>`、`tensor<Nxi1>`、`tensor<Nx!tt.ptr<i32>>` |

广播规则（与 Triton 的 splat 语义一致）：

| 左 | 右 | 结果 |
| --- | --- | --- |
| `i32` | `i32` | 标量运算 |
| `tensor<T>` | `i32` | 标量 `tt.splat` 到 `tensor<T>` |
| `i32` | `tensor<T>` | 同上 |
| `tensor<T>` | `tensor<T>` | 逐元素运算 |
| `!tt.ptr<i32>` | `i32` | `tt.addptr` |
| `tensor<Nxptr>` | `tensor<Nxi32>` | `tt.addptr` |

指针只允许 `+`：`x_ptr + offs` 先 splat 指针，再 `tt.addptr`；它绝不会被
错误地下降成整数 `arith.addi`。门禁会明确检查这一点。

比较运算在 tensor 上产生 `tensor<Nxi1>`：

```mlir
%mask = arith.cmpi slt, %offs, %n_splat : tensor<1024xi32>
```

## 语句与调用

新增的 `tl.*` 调用：

| Python | TTIR |
| --- | --- |
| `tl.program_id(0)` | `tt.get_program_id x : i32` |
| `tl.arange(0, BLOCK)` | `tt.make_range {start = 0 : i32, end = BLOCK : i32}` |
| `tl.load(ptr, mask=m)` | `tt.load %ptr, %m : tensor<Nx!tt.ptr<i32>>` |
| `tl.store(ptr, v, mask=m)` | `tt.store %ptr, %v, %m : tensor<Nx!tt.ptr<i32>>` |

约束：

- `program_id` 只支持 axis 0（`y`/`z` 留给多 CTA 的扩展）；
- `arange` 的 `start`/`end` 必须是编译期整数，且 `end > start`；
- `load`/`store` 目前只接受指针 tensor；
- mask 可以是 `i1`（自动 splat）或 `tensor<Nxi1>`，shape 必须与指针一致；
- `other=`、cache 修饰符、atomic 不在本章范围；
- `tl.sum`/`tl.max` 报错并指向第 26 章。

## 生成的 TTIR

`add_i32.py` 在 `BLOCK=1024`、三个 `ptr` 参数下的输出（节选）：

```mlir
module {
  tt.func public @add_kernel(%x_ptr: !tt.ptr<i32>, %y_ptr: !tt.ptr<i32>, %out_ptr: !tt.ptr<i32>, %n: i32) {
    %0 = arith.constant 1024 : i32
    %pid = tt.get_program_id x : i32
    %2 = arith.muli %pid, %0 : i32
    %3 = tt.make_range {start = 0 : i32, end = 1024 : i32} : tensor<1024xi32>
    %4 = tt.splat %2 : i32 -> tensor<1024xi32>
    %offs = arith.addi %4, %3 : tensor<1024xi32>
    %6 = tt.splat %n : i32 -> tensor<1024xi32>
    %mask = arith.cmpi slt, %offs, %6 : tensor<1024xi32>
    %8 = tt.splat %x_ptr : !tt.ptr<i32> -> tensor<1024x!tt.ptr<i32>>
    %9 = tt.addptr %8, %offs : tensor<1024x!tt.ptr<i32>>, tensor<1024xi32>
    %x = tt.load %9, %mask : tensor<1024x!tt.ptr<i32>>
    ...
    tt.store %15, %16, %mask : tensor<1024x!tt.ptr<i32>>
    tt.return
  }
}
```

SSA 名字（`%offs`、`%mask`、`%x`）来自 Python 变量名；重名时追加 `_N`。
canonicalize 会把它们统一改成 `%argN`/`%N`，所以命名不影响对拍。

## 与 Triton 对拍

`scripts/triton/add_i32.ttir` 是本章的 golden canonical 形式。门禁在
`TRITON_OPT` 可用时：

1. `triton-opt` 解析我们的输出；
2. 两边各跑一次 canonicalize；
3. 全文 `diff`，必须为空。

canonicalize 会重排 `make_range` 的属性顺序、统一 SSA 名称，并去掉本章
不生成的 `loc(...)`/`tt.divisibility`/`noinline`。参考形状来自 Triton 3.9
的 `demo_artifacts/add_kernel.ttir`（同一套 op 序列，指针为 f32）；本机
libtriton 是 CPython 3.12 构建，无法用 Python 前端现场生成 i32 版本，因此
golden 以 canonical 文本形式固定在仓库里。

## 实现文件

| 文件 | 作用 |
| --- | --- |
| `TTIR.{h,cpp}` | 新增 `GetProgramId/MakeRange/Splat/AddPtr/Load/Store` 与 printer |
| `Frontend.{h,cpp}` | `KernelBindings`、广播规则、指针加法、mask 检查 |
| `gpumin-triton.cpp` | `--constexpr`、`--arg-type` |
| `scripts/triton/add_i32.py` | 本章输入 |
| `scripts/triton/add_i32.ttir` | canonical golden |
| `scripts/check-ch23.sh` | 本章门禁 |

## 下一章

第 24 章把 TTIR 变成 LLVM IR：

- 固定布局：一个 program = 一个 block，`threadsPerWarp=8`、`warpsPerCTA=4`；
- `tid` 与 `elem = r*32 + tid` 的标量化；
- `tt.load/store` → `llvm.gpumin.*` 与 `addrspace(1)`；
- mask → 分支 + `phi 0`；
- 最终让 i32 vecadd 在 `gpumin_sim.py` 上跑通。
