# 第 36 章：2D mask 广播与 `other=`

## 目标

第 23 章起 `tl.load`/`tl.store` 就接受 `mask=`，但有一条硬限制：mask 张量
必须和指针形状**完全相同**，`other=` 则直接报“以后再说”。本章补上真实
Triton 的两条语义：

- mask 是 `i1` 张量时，可以广播到指针形状（`[1,8]`、`[8,1]`、`[1,1]`）；
- `tt.load` 增加第三操作数 `other`：标量自动 splat，张量必须广播到结果
  形状，元素类型必须与 pointee 一致。

示例是 ch32 attention 的 causal 版本。唯一新增的 tile 是加性 mask：

```python
r = tl.arange(0, 8)[:, None]
c = tl.arange(0, 8)[None, :]
acc = tl.zeros((8, 8), tl.float32)
for t in tl.static_range(0, 8):
    q = tl.load(q_ptr + r * 8 + t)
    kk = tl.load(k_ptr + c * 8 + t)
    acc = acc + q * kk
# p[0] 由 launch descriptor 填 0；c > r 的位置取 other。
bias = tl.load(p_ptr + r * 0 + c * 0, mask=c <= r, other=-1.0e9)
acc = acc + bias
```

`bias` 在允许的位置读 `p[0] = 0`，在未来位置取 `-1.0e9`。加上去之后，
`exp2` 对 `-1e9 * log2(e)` 下溢到 0，于是每一行只对 `c <= r` 的 key 做
softmax——不需要 inf，也没有 NaN。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/check-ch36.sh <llvm-bin-dir>          # 5/5
TRITON_OPT=<triton-opt> \
  llvm/lib/Target/GPUMin/scripts/triton-verify-all.sh <llvm-bin-dir> # 7/7
```

## TTIR：第三操作数

`tt.load` 的第三个操作数是 Triton 3.9 的原生语法，本章不发明新 op：

```mlir
%bias = tt.load %ptr, %mask, %other : tensor<8x8x!tt.ptr<f32>>
```

`%mask` 可以是 `[1,8]` 这样的行向量，前端会先补一条 `tt.broadcast`：

```mlir
%15 = tt.broadcast %colmask : tensor<1x8xi1> -> tensor<8x8xi1>
%x = tt.load %14, %15, %17 : tensor<8x8x!tt.ptr<f32>>
```

规则只有一张表：

| mask 形状 | 指针形状 | 结果 |
| --- | --- | --- |
| 标量 `i1` | 标量/张量 | splat 到指针形状 |
| 相同 | 相同 | 直接使用 |
| `[1,8]` / `[8,1]` / `[1,1]` | `[8,8]` | `tt.broadcast` 到 `[8,8]` |
| `[4,8]` | `[8,8]` | 报错：`mask shape is not broadcast-compatible with the pointer` |
| `[8,8]` | `[1,8]` | 报错：不允许把指针放大 |

`other` 只有和 `mask` 同时出现才合法；没有 mask 的 `other` 是一个静默的
死参数，前端直接拒绝。

## 降级：phi 的 else 块

`maskedLoad` 原本把 else 值写死成零：

```cpp
Phi->addIncoming(Constant::getNullValue(ElemTy), Before);
```

本章给它加一个 `Other` 参数，else incoming 换成前端给的值：

```cpp
Phi->addIncoming(Other ? Other : Constant::getNullValue(ElemTy), Before);
```

causal kernel 每个线程有 2 个 score 元素，所以 LLVM IR 里正好两条
`phi`，else incoming 是 `-1.000000e+09`；广播 kernel 是 `0.000000e+00`：

```llvm
%197 = phi float [ %196, %load ], [ -1.000000e+09, %entry ]
```

`Other` 在 `maskedLoad` 之前求值，插入点就是 `Before`，支配关系天然成立；
`-verify-machineinstrs` 会兜底。

## 为什么是 `-1.0e9` 而不是 `-inf`

真实 Triton 的 causal attention 写 `other=-float("inf")`。GPUMin 的 SFU 模型
`exp2_32` 用 `math.pow(2.0, a)` 实现，对 `-inf` 虽然也能得到 0，但模拟器的
零/NaN/inf 规则会让 Python 参考值更难写；而 `-1.0e9` 有两个好性质：

1. f32 在 1e9 附近的间距是 64，`-1e9 + 任何小分数` 仍然精确等于 `-1e9`，
   masked 位置的值不会被真实 score 污染；
2. `-1e9 * 1.4426950408889634 ≈ -1.44e9` 是有限 f32，`exp2(-1.44e9) = 0`
   没有溢出也没有 NaN。

于是整行 softmax 只统计 `c <= r` 的项，结果与解析的 causal attention
逐位一致。`e2e/causal/gen_expected.py` 用“逐 f32 舍入 + 列序归约”的模型
重算参考值；脚本先重放 ch32 的无 mask 版本，只有 64/64 对上才写 causal
的 `expected.json`。

## 数据

同一份 launch（1 block、32 线程、8×8 tile）：

| 内核 | issued | cycles | glb_ld | loc_ld | barriers |
| --- | --- | --- | --- | --- | --- |
| ch32 attention | 1236 | 938 | 1696 | 32 | 5 |
| ch36 causal | 1468 | 1132 | 1732 | 288 | 5 |

多出来的开销全部来自每个元素一次的 masked load（条件分支 + phi）和
`-1e9` 的物化；`bar.sync` 仍然是 4 条归约 barrier + 1 条 debug barrier，
没有新增指令。本章买的是表达能力，不是吞吐。

`e2e/mask_broadcast/` 是第二个 e2e：`mask=c < 4` 是 `[1,8]`，指针是
`[8,8]`，输出 64 个值里后四列全部是 `other=0.0`。它失败时能立刻区分
“广播路径坏了”和“causal 算法坏了”。

## 门禁

`check-ch36.sh` 覆盖 5 项：

1. causal 恰好一条三操作数 `tt.load`，广播 kernel 出现
   `tensor<1x8xi1> -> tensor<8x8xi1>`，两份 TTIR 与 golden canonical 对拍；
2. LLVM IR 里两条 `br i1` 与两条 `phi`，else incoming 分别是 `-1e9`/`0.0`；
3. 两个 kernel 都通过 `llc -verify-machineinstrs`，causal 仍是 5 条 barrier
   且没有 `mma`；
4. 两个 e2e 的 64 个输出都与 f32 参考逐位一致；
5. mask 不可广播、`other` 类型不匹配、`other` 没有 mask 三条诊断。

## 小结

本章没有动 ISA，也没有动线程布局：所有新语义都停在 frontend 与
`LLVMEmit` 的一层映射里。下一章把 ch30–36 的内核收成一张 benchmark 表，
给第四课收尾。
