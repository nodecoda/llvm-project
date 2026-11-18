# 第 33 章：运行时 `if/else`

## 目标

前 32 章的 kernel 都是“一条直线”：静态 `for` 会被完全展开，`tl.where`
是逐元素选择，从来没有“这一块线程走这条路、那一块走另一条路”的控制流。
本章给 TTIR-min 加上第一个**运行时**控制流结构：

```python
@triton.jit
def ifelse_kernel(x_ptr, y_ptr, n):
    pid = tl.program_id(0)
    i = tl.arange(0, 32)
    v = tl.load(x_ptr + pid * 32 + i)
    if pid < n:
        tl.store(y_ptr + pid * 32 + i, v + 1.0)
    else:
        tl.store(y_ptr + pid * 32 + i, v * 2.0)
```

`grid = 2`、`n = 1`：block 0 走 then，block 1 走 else。条件不是常量，
编译器必须生成真正的分支，而不是把两条路径都算出来再选。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch33.sh <llvm-bin-dir>
TRITON_OPT=<triton-opt> \
  llvm/lib/Target/GPUMin/scripts/triton-verify-all.sh <llvm-bin-dir>
```

`check-ch33.sh` 覆盖 5 项：TTIR/canonical 对拍、谓词分支与两条出口、
then/else 的逐位 e2e、无 `else` 时未命中的 tile 保持填充值、以及三个
作用域诊断。

## 为什么打印 `scf.if`

Triton 3.9 自己把运行时 `if` 打印成结构化控制流，而不是 `cf.cond_br`
加基本块：

```mlir
%8 = arith.cmpi slt, %pid, %n : i32
scf.if %8 {
  ...
} else {
  ...
}
```

所以本章不发明新语法，直接复用 MLIR 的 `scf.if`。`triton-opt` 能解析
它，也能 `--canonicalize`，gate 里不需要任何额外 flag——这一点和 ch34
计划中的 `gpumin.*` 自定义 op 不同。

TTIR-min 的数据结构只是给 `Op` 加了两个嵌套列表：

```cpp
struct Op {
  ...
  std::vector<Op> ThenOps;
  std::vector<Op> ElseOps;   // 空 = 没有 else
};
```

printer 从“遍历一个 `Ops`”改成递归的 `printOps(OS, Ops, Indent)`；函数体
只是 `printOps(OS, Ops, 4)` 的特例。嵌套 `if` 和静态 `for` 因此天然可用。

## 从 region 到基本块

`LLVMEmit` 为每个 `scf.if` 建三个块：

```text
        cond
       /    \
   ThenBB   ElseBB     （没有 else 时 ElseBB 直接指向 MergeBB）
       \    /
      MergeBB
```

关键实现点：

- 新增 `emitOps(ArrayRef<Op>)`，region 内递归调用；普通顺序代码路径不变。
- 进入 region 前快照 `Env`，离开后恢复：前端已经禁止名字逃逸，这里只是
  把“作用域真的结束了”落实成代码。
- `Defs` 和两条前置检查（未定义名字、重复定义）不能只看顶层 `Ops`，
  否则 branch 里用的值会被误判。`collectOps` 深度优先展平 region 后再检查。

### 坑：空块上的 `getTerminator()`

第一版在 region 结束时无条件看 `BB->getTerminator()` 判断“是否已经
有终止指令”。但**空基本块没有 terminator**，`getTerminator()` 会断言：

```text
BasicBlock::getTerminator called on a block with no terminator
```

正确写法是先问 `hasTerminator()`：

```cpp
if (!ThenBB->hasTerminator())
  Builder.CreateBr(MergeBB);
```

本章把三处相同写法都改成了 `hasTerminator()`。任何“可能为空”的块都
适用这个规则。

## 作用域：本章刻意收窄的三条线

运行时分支一旦允许“写回外层变量”，就需要 `scf.if` 的结果值和 PHI，
那是另一套机制。本章选择把边界画清楚：

| 写法 | 结果 |
| --- | --- |
| `if` 里给**外层已有名字**赋值 | 报错，需要 PHI |
| `if` 里新建名字、分支外使用 | 报错，名字不逃逸 |
| 条件是 `i1` 张量（逐 lane） | 报错，属于 mask 执行 |
| 分支里 `return` | 报错，破坏 region 单一出口 |
| `if` 里再写 `if` / 静态 `for` | 允许 |

三条诊断都能直接指出问题：

```text
assignment to 'x' inside if is not supported in chapter 33
the if condition must be a scalar i1 in chapter 33
return inside if/else is not supported in chapter 33
```

实现方式是：lower then 体之前快照 `Env` 和 `ConstValues`，lower 完逐个
比较指针是否被替换。这比事后扫描 AST 更可靠——名字解析只发生一次。

## 汇编形态

`llc -march=gpumin -O1 -verify-machineinstrs` 对一个 `if/else` 只产生：

```asm
	bra p0, .LBB0_2
	...
	exit
.LBB0_2:
	...
	exit
```

也就是一条谓词分支加两条收敛后的出口；`bar.sync` 为 0——普通分支不
需要屏障。`check-ch33.sh` 的第 2 项锁定这三个事实。

## e2e 数据

`x` 填充 `0..63`，`y` 从地址 256 开始 dump 64 个 f32：

```text
block 0（pid < n）：y[i] = x[i] + 1      -> 1, 2, ..., 32
block 1（else）    ：y[i] = x[i] * 2      -> 64, 66, ..., 126
```

全部是整数，f32 下逐位精确。第二个 e2e 是**没有 else** 的版本：
`grid = 3`、`n = 2`，`y` 预填 `-7.0`，第三个 block 不写，必须保持 `-7.0`。
它证明“条件为假且没有 else”时不会误写内存。

模拟器统计（2 个 block、32 线程）：

```text
issued=220 lane_instrs=1760 branches=8 cycles=188
glb_ld=256 glb_st=64 shm_ld=0 barriers=0
```

`branches=8` 来自两个 block 的条件分支；`barriers=0` 是本章与 ch31/ch32
最直观的对照——分支本身不引入同步。

## 下一步

下一章（ch34）把 attention 里“P 绕一次全局内存”换成真正的 shared tile，
并给出列/行切片接口；`if/else` 则成为后续 mask kernel 的控制流基础。
