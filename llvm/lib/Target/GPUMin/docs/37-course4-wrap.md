# 第 37 章：benchmark harness 与第四课收束

## 目标

第四课用五个章节把 TTIR 子集从“能写 elementwise”推到“能写 causal
attention”。最后一章不加语言特性，只做两件事：

1. 把 ch30–36 的内核收进一条命令：`scripts/bench-gpumin.sh`；
2. 把“这些数字不许倒退”变成门禁：`scripts/bench-baseline.txt` +
   `check-ch37.sh` 的上限断言。

## 退出条件（可执行）

```sh
llvm/lib/Target/GPUMin/scripts/bench-gpumin.sh <llvm-bin-dir>
llvm/lib/Target/GPUMin/scripts/check-ch37.sh <llvm-bin-dir>          # 5/5
llvm/lib/Target/GPUMin/scripts/gpumin-verify-all.sh <llvm-bin-dir>   # 8/8
TRITON_OPT=<triton-opt> \
  llvm/lib/Target/GPUMin/scripts/triton-verify-all.sh <llvm-bin-dir> # 7/7
```

## 一条命令

`bench-gpumin.sh` 不复制任何 launch 元数据：每个程序都是现成的
`triton/e2e/<name>/`，kernel 名、`--arg-type`、`--constexpr` 全部从
`expected.json` 读。流程是固定的三步：

```text
gpumin-triton --emit-llvm  ->  llc -march=gpumin -O1  ->  gpumin_sim.py --stats
```

输出是 markdown 表；`--write-baseline <file>` 输出同样内容的机器可读版本。
`bench-baseline.txt` 里每行是 `label issued=… cycles=… glb_ld=… shm_ld=… loc_st=…`。

| kernel | issued | cycles | glb_ld | shm_ld | loc_st |
| --- | --- | --- | --- | --- | --- |
| `vecadd` | 92 | 61 | 224 | 0 | 0 |
| `reduce` | 406 | 340 | 192 | 128 | 0 |
| `matmul-rank1` | 23936 | 14880 | 68608 | 0 | 8192 |
| `matmul-dot` | 54528 | 41088 | 9728 | 49152 | 74240 |
| `softmax` | 756 | 602 | 96 | 1024 | 160 |
| `attention` | 1236 | 938 | 1696 | 1024 | 32 |
| `causal` | 1468 | 1132 | 1732 | 1024 | 288 |

## 指标读法

| 指标 | 含义 |
| --- | --- |
| `issued` | warp 级发射槽总数（`lane_instrs = 8 × issued`） |
| `cycles` | 第 18 章调度模型算出的周期数 |
| `glb_ld` | 全局内存读事务（`ld.global` 及其展开） |
| `shm_ld` | 共享内存读事务（`ld.shared`） |
| `loc_st` | local store 数，作为 spill 代理 |

两个注意点：

- **不同行的 launch 规模不同**，跨行比较只有同 launch 的对子有意义：
  `matmul-rank1` vs `matmul-dot`（同一 4×4 blocks、32³），
  `attention` vs `causal`（同一 8×8 tile）。
- 门禁比较的是 `current <= baseline`，不是逐字相等。模拟器是确定性的，
  所以相等本来就会成立；用上限是为了让后来者可以优化，但不能偷偷退化。

同 launch 的两组对比正是第四课的主线：

```text
matmul-rank1  glb_ld 68608   shm_ld     0
matmul-dot    glb_ld  9728   shm_ld 49152     # shared staging 把全局流量降到 1/7

attention     issued  1236   loc_st    32
causal        issued  1468   loc_st   288     # mask 的代价：每个元素一次分支 + phi
```

## 第四课能力矩阵

| 章 | 能力 | 新增表面 | 关键证据 |
| --- | --- | --- | --- |
| 33 | 运行时 `if/else` | `OpKind::ScfIf`、嵌套 region、`scf.if` | 3 个 e2e + 分支作用域诊断 |
| 34 | shared tile 与行/列切片 | `gpumin.shared_*`、自动 barrier | `glb_ld` 50688 → 9728（ch30 同 launch） |
| 35 | `tl.dot` | `OpKind::Dot`、`tt.dot` 打印、`inputPrecision = tf32` | K 循环消失，`mma = 0` |
| 36 | mask 广播与 `other=` | `tt.load` 第三操作数、phi else 值 | causal attention 64 值逐位一致 |
| 37 | benchmark harness | `bench-gpumin.sh`、`bench-baseline.txt` | 7 行表 + 上限门禁 |

## 与第三课的对照

第三课（ch30–32）证明“手写循环也能得到正确结果”：rank-1 matmul、2D
归约 softmax、attention 都在 TTIR 里显式展开。第四课把同样的算法往
**后端**推了一步：

| 维度 | 第三课 | 第四课 |
| --- | --- | --- |
| 矩阵乘 | 32 步 rank-1 循环留在 TTIR | `tl.dot` 收敛成一条 op，K 循环下沉到 `LLVMEmit` |
| 数据搬运 | 直接 `ld.global` | shared staging + barrier，全局流量降一个数量级 |
| 控制流 | 只有 `for`/`static_range` | 运行时 `if/else`（`scf.if`） |
| 边界 | mask 形状必须完全一致 | mask 广播 + `other=`，能写 causal attention |
| ISA | 49 条指令 | **仍是 49 条**：共享内存、dot、mask 全在既有指令上展开 |

## 仍然不做

- 没有 MMA / TMA / async copy / warp specialization；
- 没有 dynamic shared memory，也没有 shared 指针类型（`tl.alloc_shared` 返回句柄）；
- 没有 bf16/fp16、稀疏、量化；
- 不跑真实 Triton 管线：只用 `triton-opt` 做 TTIR 解析与 canonical 对拍；
- 不做性能调优。benchmark 是**回归锁**，不是性能承诺；`cycles` 来自教学
  调度模型，不能和真实 GPU 对比。

## 更新 baseline

只有在确认改动是改进（或有意变更）时才更新基线：

```sh
llvm/lib/Target/GPUMin/scripts/bench-gpumin.sh <bin> --write-baseline /tmp/bench.raw
# 把 /tmp/bench.raw 的 7 行贴到 bench-baseline.txt 的注释之后
llvm/lib/Target/GPUMin/scripts/check-ch37.sh <bin>
```

`check-ch37.sh` 还会检查两条结构性不变量：`matmul-dot` 必须走 shared
（`shm_ld > 0` 且 `matmul-rank1` 的 `shm_ld == 0`），`causal` 必须比
`attention` 多花发射槽（mask 不是免费的）。数字可以变小，这两条不能消失。
