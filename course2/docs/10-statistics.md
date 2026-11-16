# 第 10 章：指令统计与“性能”

## 目标

给模拟器加一个 `--stats`：程序结束后按助记符输出**退休指令直方图**，
并保证三件事互相印证：

- 直方图首行的 `instructions` == 各行计数之和；
- 它 == 第 05 章摘要里的“after N instructions”；
- 前 5 行与独立聚合 `--trace` 得到的结果逐字一致。

这一章不实现流水线，也不假装有缓存：**周期 = 退休指令数**。
先把“数得准”做好，后面的性能模型才有地基。

## 背景：为什么统计值得单独一章

前九章证明了“跑得对”，但没证明“跑得一样多”。统计是最便宜的可观测性：

| 问题 | 直方图能回答 |
| --- | --- |
| `tail` 的尾调用真的发生了吗？ | `jalr` 计数 ≈ 2×10⁶（两对互递归） |
| `-O0` 与 `-O2` 差在哪？ | `lw/sw/addi` 的占比变化 |
| trace 与执行器口径一致吗？ | top-5 交叉验证 |
| 指令集扩展被用上了吗？ | `mul/div`、`clz` 的计数是否为 0 |

两个设计决定：

1. **统计放在 CPU 里**，而不是“运行后重新译码一遍”：只有执行器知道
   哪些指令真的退休了（分支不跳、trap、`mret` 都影响结果）；
2. **输出走 stderr**：第 05 章起 stdout 是 guest 的私有财产，要和 QEMU
   逐字节 diff；`--trace` 已经在 stderr，统计也照此办理。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `rsim/stats.h` | `Stats::add/count/total/print`，按 `Insn` 编号开桶 |
| `rsim/decode.h` | `Insn::Count` 哨兵 + `kInsnCount` |
| `rsim/cpu.h` | `Stats stats_` 成员与 `stats()` 访问器 |
| `rsim/cpu.cpp` | `++executed_` 之后 `stats_.add(d.id)` |
| `rsim/main.cpp` | `--stats` 解析与打印 |
| `scripts/course2-verify.sh` | tail 的 `jalr` 阈值、三口径一致、top-5 交叉验证 |

## 关键实现

### 1. 桶的下标就是 `Insn`

```cpp
void add(Insn id) { ++counts_[static_cast<unsigned>(id)]; }
```

`insn.def` 是唯一真源，新增一条指令自动多一个桶。为了让“桶的个数”可计算，
`decode.h` 在 X-macro 列表末尾加了哨兵：

```cpp
enum class Insn : uint16_t {
  ILLEGAL,
#define RSIM_INSN(...) ID,
#include "insn.def"
#undef RSIM_INSN
  Count, // one past the last real instruction
};
constexpr unsigned kInsnCount = static_cast<unsigned>(Insn::Count);
```

### 2. 计数点：`executed_++` 之后

```cpp
if ((isa_ & isaOf(d.id)) == 0)
  throw Trap(...);          // 被 --isa 掩掉：不算退休
++executed_;
stats_.add(d.id);           // 与 minstret 同一时刻
if (trace_) ...
```

由此得到一条可检验的不变式：**统计口径 == `minstret` == `--trace` 行数**。
非法指令在 `++executed_` 之前抛出，所以不会进桶——它没有退休。

### 3. 稳定排序

```cpp
std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
  if (a.first != b.first) return a.first > b.first;
  return std::strcmp(a.second, b.second) < 0;
});
```

次数降序、同频按助记符字典序。没有这个 tie-break，同一程序两次运行的
输出可能不同，门禁就没法逐字 diff。`LC_ALL=C` 在脚本里固定排序规则，
避免不同 locale 下 `sort` 把 `addi` 和 `and` 排反。

### 4. `--stats` 不改行为

统计在停机之后打印，且只写 stderr。门禁显式比较：

```bash
"$RSIM" --run demo > plain.out 2>/dev/null
"$RSIM" --run demo --stats > stats.out 2>/dev/null
cmp plain.out stats.out
```

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B/course2"
"$B/course2/rsim" --run <elf> --stats
"$W/course2/scripts/course2-verify.sh" "$B/bin" "$B/course2/rsim"
```

门禁从 186 项扩到 **190 项**，新增 4 项：

1. `tail` 在 `--stats` 下退出 0，`jalr ≥ 1,000,000`，且 stdout 仍是 `rvmin: ok`；
2. 直方图 `instructions` == 各行之和 == 步数计数；
3. `demo` 的 top-5 与 `--trace` 聚合结果一致；
4. `--stats` 不改变 guest 的 stdout。

实测（`-O2`，Release 构建）：`tail` 退休 **18,010,059** 条，其中 `jalr` **2,001,006** 条；
`demo` 的 top-5：

```text
addi 1087
slli 473
lw   376
sw   362
srli 223
```

## 预期输出

```text
$ rsim --run demo --stats
rvmin: ok                     # stdout（guest）
instructions 4027             # 以下 stderr
addi 1087
slli 473
lw 376
sw 362
srli 223
...
rsim: halted at 0x00010040 after 4027 instructions (exit 0, a0=0x00000000, t0=0x00000000)

$ rsim --run tail --max-steps 50000000 --stats 2>&1 | grep -E '^(instructions|jalr) '
instructions 18010059
jalr 2001006
```

## 常见坑

1. **把统计打到 stdout**。guest 的输出立刻被污染，第 05 章的 QEMU 逐字节
   差分全红；`--trace` 早就踩过一次，统计必须走 stderr。
2. **用 `ILLEGAL + 1` 当桶数**。`ILLEGAL` 是 0 号，真正的末项是最后一条
   指令；不加 `Count` 哨兵会数组越界或漏计。
3. **在 `--isa` 检查之前计数**。被掩掉的指令会“假装退休”，直方图与
   `minstret` 对不上；计数点必须在 `++executed_` 之后。
4. **忘记 tie-break**。同频指令的顺序取决于 `std::sort` 的内部实现，
   换个编译器门禁就假红。
5. **拿 `--trace` 统计 `tail`**。1800 万行 × 每行几十字节 ≈ 1 GB；
   小程序的 top-5 已经足够做交叉验证。
6. **以为“周期 = 指令数”是性能结论**。它只是一个约定；真正的 CPI、
   流水线冒险、缓存命中都要另开模型，文档里不要混为一谈。

## 练习

1. 给 `--stats` 加 `--stats-top N`，只打印前 N 行（默认全部）。
2. 增加按**指令类别**（load/store/branch/ALU）聚合的第二张表。
3. 用 `-O0` 与 `-O2` 各跑一次 `large-frame`，把两张直方图做差，
   解释 `addi`/`lw` 的变化。
4. 让 `--stats` 同时输出 `minstret` 与 `mcycle`（本章二者相等），
   为第 11 章之后的周期模型留接口。
5. 把直方图按 ISA 扩展分组（RV32I/M/Zbb/Zbs/A），验证 `demo` 的 A 组为 0。
