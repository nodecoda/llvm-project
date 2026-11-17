# 第 56 章：第七篇验收——向量进阶教学子集

## 目标

第 52–55 章分别交付了向量 mask、LMUL=2、Zve32f 和 LMUL=1 向量 ABI。
本章不加新指令、不改 ABI，只回答一个问题：

> 第七篇承诺的“向量进阶教学子集”，能不能用可执行证据逐条兑现？

完成之后：

1. 52–55 的验收项全部有测试文件或命令；
2. 计数锁定为 **29 文件 / 6 not llc / 276 llc / 42 lit / 41 V 语料 /
   432 clang / 288 e2e**；
3. 默认八项门禁仍全绿；
4. `rvmin-run-v.sh` 在 QEMU 6.2 上明确返回“环境缺 V”，不冒充编译器失败；
5. 文档与索引固定口径：**不是完整 RVV，不声称 `rv32gcv` 全量支持**。

---

## 第七篇交付总览

| 章 | 主题 | 提交 | tag |
| --- | --- | --- | --- |
| 52 | 向量 mask：`vm` 位、v0、6 条 masked ALU、`vmerge.vvm` | `13f79602aedd` | `rvmin-ch52` |
| 53 | LMUL=2：寄存器对、`v8i32`、m2 load/store/add、`vmv2r.v` | `6ae049aa4f75` | `rvmin-ch53` |
| 54 | Zve32f：`v4f32`、`vfadd/vfsub/vfmul`、`vfmv` | `4c2f1bbcfe00` | `rvmin-ch54` |
| 55 | 向量 ABI：v8–v23、第 17 个参数进栈、向量 callee-saved | `62c8bcebbcae` | `rvmin-ch55` |

每章都是“中文八要素文档 + 1 commit + 1 tag + 推 origin”。
`git checkout rvmin-ch55` 可以看到第七篇结束时的完整代码。

---

## 验收表

### 第 52 章：向量 mask

| 编号 | 条件 | 证据 |
| --- | --- | --- |
| AC52-1 | 7 条 mask 指令编码与 `riscv32 +v` 逐字节一致 | `check-rvmin-encodings.sh` |
| AC52-2 | 反汇编往返一致 | `check-rvmin-disasm.sh` |
| AC52-3 | 4 类 mask 拼写错误 + 2 类 vd=v0 错误 | `v-extension.s` |
| AC52-4 | `select <4 x i1>` 常量 mask 生成 `vmv.v.x v0` + `vmerge.vvm` | `v-ext-mask.ll` |
| AC52-5 | MIR 中 true 值在 `vs1` | `v-ext-mask.ll` |
| AC52-6 | 非常量 mask 明确拒绝 | `v-ext-mask.ll` 的 `RUN: not llc` |
| AC52-7 | lit 37/37、8/8、llc 216、clang 432、e2e 288 | 第 52 章门禁输出 |
| AC52-8 | DSL 契约 OK | `dsl-surface.py --check` |

### 第 53 章：LMUL=2

| 编号 | 条件 | 证据 |
| --- | --- | --- |
| AC53-1 | `v8i32` load/store/add 生成 `vsetivli e32,m2` + m2 指令 | `v-lmul2.ll` |
| AC53-2 | 20 个 `v8i32` 值能 spill/reload，32 字节槽 | `v-lmul2.ll` + `-verify-machineinstrs` |
| AC53-3 | `vmv2r.v` 编码与 `riscv32 +v` 一致 | `v-extension.s`、语料 36 |
| AC53-4 | `vmv2r.v` 奇数 vd/vs2 报错 | `v-extension.s` 负例 |
| AC53-5 | m1/m2 同块混用发两次 `vsetivli` | `v-lmul2.ll` |
| AC53-6 | 循环 header/body 各自有正确配置 | `v-lmul2.ll` |
| AC53-7 | llc 228、lit 38、8/8、432、288 | 第 53 章门禁输出 |
| AC53-8 | DSL 契约 7 后端 OK | `dsl-surface.py --check` |

### 第 54 章：Zve32f

| 编号 | 条件 | 证据 |
| --- | --- | --- |
| AC54-1 | `+v,+f` 下 `v4f32` 的 add/sub/mul 生成三条 `vf*` | `v-fp.ll` |
| AC54-2 | 三条编码与 `riscv32 +v,+f` 逐字节一致 | `check-rvmin-encodings.sh` |
| AC54-3 | `vfmv.v.f`/`vfmv.f.s` 的 FPR 边界正确 | `v-fp.s`、`v-fp.ll` |
| AC54-4 | 无反汇编往返差异 | `check-rvmin-disasm.sh` |
| AC54-5 | 无 `+f` 时不生成 `vf*`，退回 `__addsf3` | `v-fp.ll` 的 `NOF` |
| AC54-6 | lit 40、语料 41、240 llc、432、288 | 第 54 章门禁输出 |
| AC54-7 | DSL 契约 7 后端 OK | `dsl-surface.py --check` |

### 第 55 章：LMUL=1 向量 ABI

| 编号 | 条件 | 证据 |
| --- | --- | --- |
| AC55-1 | `<4 x i32>` 参数进入 v8 | `v-ext-abi-arg.ll` 的 `vse32.v v8` |
| AC55-2 | 17 个参数时第 17 个从栈加载 | `v-ext-abi-arg.ll` 的 `arg17` |
| AC55-3 | `<4 x i32>` 返回在 v8 | `v-ext-abi-ret.ll`、`v-abi.ll` |
| AC55-4 | `v8i32` 参数/返回仍被拒绝 | `v-abi-m2.ll` |
| AC55-5 | `v4f32` 仍被通用诊断拒绝 | 手工 `llc` 命令 |
| AC55-6 | v1–v7、v24–v31 按需保存/恢复 | `v-abi.ll` 的 `callee_saved` |
| AC55-7 | `+v,+f` 同时保存 FPR 与 VR | `v-abi.ll` 的 `fp_v_csr` |
| AC55-8 | mask+m2 混合不破坏 v0 | `v-ext-mask.ll` |
| AC55-9 | 42 lit、29 文件、6 not llc、276 llc | 门禁输出 |
| AC55-10 | V 语料 41、编码/反汇编/432/288 不回归 | 两个脚本 + clang 矩阵 |
| AC55-11 | m2 值跨调用时 V24/V25 两半都保存 | `v-abi.ll` 的 `m2_csr` |
| AC55-12 | `+v,+f` 的 lit 断言 | `v-abi.ll` 的 `FPV` |
| AC55-13 | 10 条 DSL 契约 OK | `dsl-surface.py --check` |
| AC55-14 | 本章八要素文档 | `55-vector-abi.md` |
| AC55-15 | 两个 README 有第 55 行 | diff |
| AC55-16 | `rvmin-ch55` 已推 origin | `git tag` / `git push` |

---

## 计数与口径

| 项 | 第 54 章 | 第 55/56 章 |
| --- | --- | --- |
| lit（CodeGen+MC/RVMin） | 40 | **42** |
| CodeGen 文件 / not llc | 27 / 6 | **29 / 6** |
| `llc` 门禁用例 | 240 | **276** |
| V 编码语料 | 41 | **41** |
| V 反汇编往返 | 41 | **41** |
| clang 矩阵 | 432 | **432** |
| e2e 对象 | 288 | **288** |
| V 静态 e2e | 4 | **4** |
| 默认门禁 | 8/8 | **8/8** |
| DSL 契约 | 10 条 | **10 条** |

公式：

```text
llc = (CodeGen/RVMin/*.ll 文件数 - RUN: not llc 文件数) × 4 个 -O × 3 个 mattr
    = (29 - 6) × 4 × 3
    = 276
```

第 55 章没有新增 V 指令，所以编码/反汇编语料保持 41；
`v-ext-abi-arg.ll`、`v-ext-abi-ret.ll` 从负例翻成正例，新增 `v-abi.ll`，
`v-abi-m2.ll` 仍带 `RUN: not llc`，正好把文件增量抵消为 29/6。

---

## 构建与验证

```sh
ninja -C /home/ubuntu/llvm/builds/pr-ctpop llc llvm-mc llvm-objdump clang
llvm-lit -s llvm/test/CodeGen/RVMin llvm/test/MC/RVMin
llvm/lib/Target/RVMin/scripts/check-rvmin-encodings.sh /home/ubuntu/llvm/builds/pr-ctpop/bin
llvm/lib/Target/RVMin/scripts/check-rvmin-disasm.sh /home/ubuntu/llvm/builds/pr-ctpop/bin
llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh /home/ubuntu/llvm/builds/pr-ctpop/bin
llvm/lib/Target/RVMin/scripts/rvmin-clang-matrix.sh /home/ubuntu/llvm/builds/pr-ctpop/bin
```

本章实测：

```text
lit: 42/42
PASS: ... + 41 V instructions, .text identical
PASS: v (41 instructions, .text round-trips byte-for-byte)
PASS 5/8: e2e (+ V subset)           # 288 + 4 对象
PASS 6/8: 276 llc runs with -verify-machineinstrs
PASS 7/8: TableGen DSL contracts     # 10 条 OK
ALL 8 GATES PASS
ALL CLANG MATRIX PASS: 432 objects
```

---

## QEMU 环境缺口

默认门禁第 8 项只跑 RV32I/M/A + F/D + C，使用系统
`qemu-riscv32-static`。本章环境是 **QEMU 6.2**，它不实现 V：

```text
$ llvm/lib/Target/RVMin/scripts/rvmin-run-v.sh /home/ubuntu/llvm/builds/pr-ctpop/bin
SKIP: qemu-riscv32-static does not implement the V extension (SIGILL); use QEMU >= 8.2
$ echo $?
2
```

`rvmin-run-v.sh` 用退出码 2 区分“环境缺 V”和真正的编译/链接/运行失败。
第 50 章的 QEMU 8.2 运行证据仍然有效；本章**没有**在 QEMU 6.2 上重跑 V，
也不把 SIGILL 说成编译器错误。

---

## 教学子集边界

第七篇结束时，RVMin 的向量支持是：

| 维度 | 支持 | 不支持 |
| --- | --- | --- |
| SEW | 32（Zve32x + Zve32f） | 8/16 |
| LMUL | 1、2 | 4、8 |
| VLEN | 128 | 其他 VLEN |
| 固定类型 | `v4i32`、`v4i1`、`v4f32`、`v8i32` | `v8f32`、`v16i8`、`v8i16`、scalable vector |
| mask | 常量 mask、v0、6 条 masked ALU、`vmerge.vvm` | 非常量 mask、mask 逻辑指令 |
| load/store | unit-stride `vle32.v`/`vse32.v`、m1/m2 | strided、indexed、segment |
| 整数 ALU | `vadd/vsub/vmul/vand/vor/vxor` 的 `.vv/.vi/.vx` | widening/narrowing、div/rem、饱和 |
| 浮点 ALU | `vfadd/vfsub/vfmul`、`vfmv.v.f`/`vfmv.f.s` | `vfdiv/vfsqrt`、`vfredsum`、masked `vf*` |
| 归约 | `vredsum.vs` | 其他归约 |
| ABI | `<4 x i32>` 的 LMUL=1 调用 | `v8i32`、`v4f32`、varargs、sret 向量 |
| 配置 | 保守 `vsetivli` | `vsetvli` 优化、冗余消除 |
| 运行 | QEMU 8.2（第 50 章证据） | 默认 QEMU 6.2/7.2（SIGILL） |

因此：

- **不是完整 RVV**；
- **不声称 `rv32gcv` 全量支持**；
- `rv32imafdv` 能编译/链接/静态检查，不等于所有 RVV 程序都能运行。

---

## 与完整 RVV 的差距

1. **元素宽度**：只有 SEW=32；没有 8/16 位元素。
2. **LMUL**：只有 1/2；没有 4/8，也没有 fractional LMUL。
3. **mask**：只支持常量 mask；`icmp` 产生的运行时 mask 仍被拒绝。
4. **内存**：只有 unit-stride；没有 strided/indexed/segment。
5. **算术**：没有 widening/narrowing、div/rem、饱和、浮点 div/sqrt。
6. **归约**：只有 `vredsum.vs`；没有 `vfred*`、`vredmax/min`。
7. **ABI**：只有 `v4i32`；`v8i32` 和 `v4f32` 都明确拒绝。
8. **配置**：`vsetivli` 保守发射；没有 `vsetvli` 优化和冗余消除。
9. **类型系统**：没有 scalable vector（`vscale`），只有固定向量。
10. **运行环境**：默认 QEMU 6.2 不支持 V；需要 QEMU ≥ 8.2。

---

## 常见坑

1. **计数不是“文件数 × 12”。** 必须先减去 `RUN: not llc` 的文件：
   `(29 - 6) × 4 × 3 = 276`。把负例算进去会得到 348。
2. **第 55 章没有新增指令。** V 编码/反汇编语料仍是 41；
   不要因为新增 ABI 测试就把语料改成 43/45。
3. **`v4f32` 不是 ABI 类型。** 第 54 章只让它在寄存器内做算术；
   跨调用仍报 `vector arguments are not supported yet`。
4. **m2 的 callee-saved 靠两半保存。** `CSR_RVMin_V` 没有 `V24M2`；
   PEI 的 alias-aware 检查会同时保存 V24/V25。两条 m1 store 是优化空间。
5. **VR 没有 CFI。** 向量 spill 不生成 `.cfi_offset`；
   这不影响运行，但 unwinder 看不到向量寄存器。
6. **QEMU 6.2 的 SIGILL 是环境缺口。** `rvmin-run-v.sh` 用退出码 2 表示
   “模拟器不支持 V”，不是编译器失败。
7. **DSL 契约是 10 条检查、7 个后端。** “7 后端”指 `--summary` 的后端数；
   不要把它和 10 条 `--check` 混为一谈。
8. **README 的两处口径要同时改。** `docs/README.md` 与
   `llvm/lib/Target/RVMin/README.md` 都有“V 只覆盖 …”的段落；
   第 52–55 章之后必须更新，否则读者会以为仍然“无 mask、无向量 FP、无 ABI”。

---

## 练习

1. 从 `rvmin-ch54` 和 `rvmin-ch55` 各 checkout 一次，比较
   `git diff rvmin-ch54..rvmin-ch55 --stat`，确认第 55 章只改 ABI/CSR/文档，
   没有新增编码语料。
2. 手工跑一遍 §“构建与验证”的六条命令，把输出与本章的计数表对照。
3. 在 QEMU 8.2 环境重跑 `rvmin-run-v.sh`，确认退出码从 2 变成 0。
4. 给第 55 章新增一个“第 18 个向量参数”的用例，确认它落在 `16(sp)`；
   如果寄存器分配顺序改变，检查 `CCAssignToStack<16,16>` 的对齐。
5. 选一个“与完整 RVV 的差距”条目，写一页设计草案：
   非常量 mask、SEW=8/16、m2 ABI 或 mask 逻辑指令，任选其一。
