# 第 4 章：TTIR → TTGIR —— 给每个张量分配线程

第 3 章手工写了一份带布局的 TTGIR。这一章把这件事自动化：

```sh
tiny-opt kernels/vecadd.ttir --tiny-convert-tt-to-ttg -o vecadd.ttgir
```

转换只做两件事：

1. 每个没有 encoding 的 tensor 补上默认 blocked 布局；
2. 每个 `tinytt.*` op 换成同名的 `tinyttg.*` op。

它**不是 lowering**：没有引入循环、没有标量化、没有碰内存。SSA 图完全
不变，变的只有类型和 dialect 名。

## 1. 为什么这章不用 dialect conversion

MLIR 的 dialect conversion 框架适合“值会被替代、可能插入 materialization、
需要来回尝试 pattern”的场景。本章三者都不是：没有新值，没有值被删除，
每个 op 都必然成功。

因此本章的做法更直接：

```text
1. 改 func.func 签名与 entry block 参数类型
2. 原地改每个 tensor result 的类型（all uses 自动看到新类型）
3. 把 tinytt op 逐个重建成 tinyttg op
4. module 上写 tinyttg.num_warps
```

第 5 章要进入 LLVM dialect，值会被拆分、替换，那时才引入 `TypeConverter`。
提前用重型框架只会让读者以为“布局赋值需要转换框架”。

## 2. TargetInfo：8/4 只出现一次

需求 I5 要求 conversion pattern 里不出现魔法数。本章把它收口到：

```cpp
struct TargetInfo {
  int64_t lanesPerWarp = 8;
  int64_t warpsPerCTA = 4;
  int64_t threadsPerBlock() const { return lanesPerWarp * warpsPerCTA; }
  ttg::BlockedAttr getDefaultBlocked(MLIRContext *, int64_t rank) const;
};
```

- 1D：`sizePerThread=[1] threadsPerWarp=[8] warpsPerCTA=[4] order=[0]`
- 2D：`sizePerThread=[1,1] threadsPerWarp=[1,8] warpsPerCTA=[4,1] order=[1,0]`

`8` 与 `4` 来自 GPUMin 的 SIMT 模型（8 lane/warp、4 warp/block），不是本章
新造的常量。改目标只改 `TargetInfo`。

## 3. 布局覆盖是整除，不是相等

`tensor<1024xi32>` 与 32 线程的 tile：

```text
1024 / (1 * 8 * 4) = 32 次重复
```

所以 pass 的检查是

```cpp
extent % blocked.getTileSize(dim) == 0
```

不满足就报错，而不是插 mask：

```text
error: tensor dimension 0 of size 100 is not divisible by the 32 threads
       that one tile covers
```

这正是 `tests/negative/ttg-convert-bad-size.ttir` 锁住的行为。Triton 对
不整除的 shape 会引入 mask；tiny 一期明确不做，理由与第 2 章一致：宁可
报错，不静默降级。

## 4. 十二个 op 怎么换 dialect

没有泛型“改个前缀”的 API。每个 op 用 typed accessor 读属性，再用
目标 op 的 builder 重建：

```cpp
if (auto old = dyn_cast<tiny::tt::SplatOp>(op)) {
  auto newOp = builder.create<tiny::ttg::SplatOp>(
      loc, old.getResult().getType(), old.getSrc());
  return finish(newOp);          // replaceAllUsesWith + erase
}
```

`ReduceOp` 的 `axis/combine`、`TransOp` 的 `order` 都通过
`getAxisAttr()/getCombineAttr()/getOrderAttr()` 传递，属性不会在重建时丢失。
十二个分支全部是机械的，这也是第 3 章把 op 定义抽到 `TinyCommon` 的回报：
两个 dialect 的操作数顺序天然一致。

## 5. 让 TTGIR 可读：布局别名

如果每个 tensor 都内联打印布局，vecadd 的每一行都会重复 90 个字符。
本章给 `tinyttg` 加一个 `OpAsmDialectInterface`：

```mlir
#blocked = #tinyttg.blocked<sizePerThread = [1], threadsPerWarp = [8],
                            warpsPerCTA = [4], order = [0]>
```

之后 TTGIR 里只剩 `tensor<1024xf32, #blocked>`。这是打印层的能力，不影响
IR 语义：alias 只是 `OpAsmPrinter` 的缩写，parse 回来仍是同一个属性。

## 6. 产物

`tests/passes/vecadd.ttgir` 是本章 golden，由 pass 直接生成。它与第 3 章
手写的 `tests/ttg/vecadd.ttgir` 语义相同，差别只在 alias 的写法：
手写版把四个字段折成两行，printer 输出一行。两个文件都保留，分别锁住
“手写 TTGIR 合法”和“pass 输出稳定”。

module 上的 `tinyttg.num_warps = 4` 是 TTGIR 的自描述：拿到一份 `.ttgir`
不需要再问“这是几个 warp 的目标”。

## 7. 门禁

```sh
tiny-triton/scripts/check-t4.sh /home/ubuntu/llvm/builds/pr-ctpop/bin
```

五项：

1. `--tiny-convert-tt-to-ttg` 能跑通 `kernels/vecadd.ttir`；
2. 输出与 `tests/passes/vecadd.ttgir` 逐字节一致；
3. 输出能被 `--verify-each` 接受，含 `tinyttg.addptr`，且没有残留
   `tinytt.` op（`!tinytt.ptr` 不算）；
4. pass 出现在 `--help`，并且能通过
   `--pass-pipeline='builtin.module(tiny-convert-tt-to-ttg)'` 单跑；
5. 100 元素负例被拒绝，诊断含 `not divisible`。

## 8. 退出条件

- `ninja -C <build> tiny-opt` 成功；
- `check-t4.sh` 5/5；`check-t1.sh`–`check-t3.sh` 全绿；
- 旧管线 `gpumin-verify-all.sh` 8/8、`triton-verify-all.sh` 0 failed；
- `git tag tiny-t04` 指向本章 commit。
