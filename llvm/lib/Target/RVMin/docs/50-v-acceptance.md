# 第 50 章：第六篇验收——Zve32x 子集全链路

## 目标

第 47、48、49 章分别交付了 V 的 MC 层、第一版代码生成和优化。本章不加新
指令、不改 ABI，只回答一个问题：

> 第六篇承诺的“Zve32x 教学子集”，能不能用可执行证据逐条兑现？

完成之后：

1. 编码门禁覆盖 **85 RV32I + 12 M + 19 Zbb + 8 Zbs + 19 A + 28 F + 28 D
   + 35 C + 24 Zicsr/Zifencei + 27 V**，全部与
   `llvm-mc -triple=riscv32 -mattr=+v` 逐字节相同；
2. 反汇编门禁的 V 段覆盖 **27 条**，`.text` 往返字节相同；
3. V 静态 e2e 覆盖 `v-demo.c` 的 **4 个优化级**，llc 的对象与
   `rvmin`/`riscv32` 两个汇编器的重汇编逐字节相同；
4. 默认八项门禁仍全绿；第 5 项增加 V 子证据，但 **288 对象**不变；
5. clang 矩阵仍是 **432 对象**（`rv32imafdv` 已在矩阵内）；
6. QEMU 8.2 运行 `rvmin: ok`；QEMU 6.2/7.2 的 SIGILL 作为环境缺口记录；
7. 文档与索引固定口径：**不是完整 RVV，不声称 `rv32gcv` 全量支持**。

## 背景：验收口径与三处有意选择

### 1. 为什么保持 8/8，而不是加第 9 项

计划允许“保持 8 项结构，或新增第 9 项 V codegen gate”。本章选择保持 8/8：

- 第 45 章已经把 288/432 写进多条验收与 README；改变门禁数量会让所有
  “8/8”证据失效，收益只是编号更整齐。
- V 静态检查是确定性的、便宜的，挂在第 5 项 e2e 后面即可；
- QEMU≥8 是环境相关项，放进默认门禁会让没有 QEMU 8 的机器整体失败。

因此：

```text
5  e2e：12 × 4 × 6 = 288 对象（不变）
      + V 1 × 4 = 4 对象（新增子证据）
8  QEMU：RV32I/M/A + F/D + C（仍用系统 QEMU 6.2，不含 V）
```

V 的运行放在独立脚本 `rvmin-run-v.sh`，退出码区分“环境缺 V”和“真失败”。

### 2. O0 的 `-filetype=obj` 暴露了一个真实尺寸 bug

新增静态门禁时，`llc -O0 -mattr=+f,+d,+v -filetype=obj
-verify-machineinstrs` 直接 aborted：

```text
Size mismatch for: $v8 = VLE32_V killed $x10 :: (load (s128) from %stack.9)
Expected maximum size: 4
Actual size: 8
```

原因不是编码错误，而是**尺寸报告与 AsmPrinter 的实际发射不一致**：

- `getInstSizeInBytes()` 对真实指令返回 `Desc.getSize()`，V 指令是 4；
- AsmPrinter 在缓存失效时会先发一条 4 字节 `vsetivli`，再发数据指令，
  实际增量是 8；
- 通用 AsmPrinter 的尺寸校验默认是 `AllowOverEstimate`（实际 ≤ 报告值），
  所以“报告 4、实际 8”必然失败。

修法是让 `getInstSizeInBytes()` 对“可能插入 vset 的数据向量指令”报告
上界 8，对 `vset*` 仍报告 4：

```cpp
if ((MI.getDesc().TSFlags & 0x1f) == 17) {
  unsigned Opc = MI.getOpcode();
  if (Opc != RVMin::VSETIVLI && Opc != RVMin::VSETVLI &&
      Opc != RVMin::VSETVL)
    return 8;
}
```

这不是“为了让测试过”而放宽：真实发射就是 4 或 8，8 是上界；
`AllowOverEstimate` 的语义正是“实际不超过报告值”。同时给 `v-ext.ll`
增加一条 O0 + obj + `-verify-machineinstrs` 的 RUN 行，防止回归。

### 3. QEMU 验证语义，不验证编码

QEMU 执行的是它自己解码后的语义；字节级正确性仍由编码门禁（对照
`riscv32 +v`）和反汇编往返门禁保证。两者缺一不可：

- 编码门禁证明“我们发的字节和参考汇编器一致”；
- QEMU 证明“这些字节在真实执行模型里得到正确结果”。

## 代码地图

| 位置 | 职责 |
| --- | --- |
| `scripts/rvmin-v-encoding-corpus.s` | 27 条 V 编码语料（vset/load/store/vmv/ALU/reduction） |
| `scripts/check-rvmin-encodings.sh:260` | V 段：27 条对照 `riscv32 +v` |
| `scripts/check-rvmin-disasm.sh:196` | V 段：27 条逐字节往返 |
| `scripts/rvmin-e2e-v.sh` | 本章新增：V 静态 e2e（4 个优化级） |
| `scripts/rvmin-run-v.sh` | 本章新增：QEMU≥8 运行，退出码 0/2/1 |
| `scripts/rvmin-verify-all.sh:59` | 第 5 项挂 V 静态检查，保持 8/8 |
| `scripts/e2e-v/v-demo.c` | `.vv/.vi/.vx/归约` 四个检查 |
| `RVMinInstrInfo.cpp:29` | 数据向量指令尺寸上界 8（本章修复） |
| `test/CodeGen/RVMin/v-ext.ll` | 选择/去重/折叠/归约/O0 obj 回归 |
| `test/MC/RVMin/v-extension.s` | 27 条编码 + 反汇编 |
| `docs/47-v-extension-mc.md` | V 的 MC 层 |
| `docs/48-v-codegen-1.md` | V 代码生成（一） |
| `docs/49-v-codegen-2.md` | V 代码生成（二） |
| `docs/50-v-acceptance.md` | 本章 |

## 验收表

### V 的 MC 层（第 47 章）

| ID | 标准 | 证据 | 结果 |
| --- | --- | --- | --- |
| V-MC1 | `v0`–`v31` 可作为向量寄存器 | `v-extension.s`、`v-ext.ll` | 通过 |
| V-MC2 | `vsetivli/vsetvli/vsetvl` 编码正确 | 编码语料 3 条 | 通过 |
| V-MC3 | `vle32.v/vse32.v` 编码正确 | 编码语料 2 条 | 通过 |
| V-MC4 | `vmv.v.i/v.v/x.s/s.x/v.x` 编码正确 | 编码语料 6 条 | 通过 |
| V-MC5 | `vadd/vsub/vmul` 的 `.vv/.vx/.vi` 编码正确 | 编码语料 | 通过 |
| V-MC6 | `vand/vor/vxor` 的 `.vv/.vx/.vi` 编码正确 | 编码语料 | 通过 |
| V-MC7 | `vredsum.vs` 编码正确 | 编码语料 1 条 | 通过 |
| V-MC8 | 缺 V 时汇编器给出诊断 | `v-extension.s` 的 `MISSING` 段 | 通过 |

### V 的代码生成（第 48/49 章）

| ID | 标准 | 证据 | 结果 |
| --- | --- | --- | --- |
| V-CG1 | `v4i32` load/store 选择 `vle32.v/vse32.v` | `v-ext.ll` `add4` | 通过 |
| V-CG2 | add/sub/mul/and/or/xor 选择 `.vv` | `v-ext.ll` `ops` | 通过 |
| V-CG3 | 5 位 splat 选择 `vmv.v.i` | `v-ext.ll` | 通过 |
| V-CG4 | 大常量走 16 字节栈槽 + `vle32.v` | `v-ext.ll` `splat100` | 通过 |
| V-CG5 | extract 0 用 `vmv.x.s`；1..3 走栈 | `v-ext.ll` `extract0/1` | 通过 |
| V-CG6 | 向量 spill/reload 使用 16 字节槽 | `v-ext.ll` `spill` | 通过 |
| V-CG7 | 第 48 章保守 vset（历史行为） | `docs/48` | 已被 CG12 取代 |
| V-CG8 | 输出可被 `rvmin +v` 与 `riscv32 +v` 重汇编 | 两条 RUN 行 | 通过 |
| V-CG9 | 无 `+v` 时同一 IR 可标量化编译 | 216 次 llc | 通过 |
| V-CG10 | 向量参数/返回值明确报错 | `v-ext-abi-{arg,ret}.ll` | 通过 |
| V-CG11 | 288/432/191 计数不变 | 门禁与矩阵 | 通过 |
| V-CG12 | 块内相同 VL/VTYPE 只发一次 | `v-ext.ll` `add4` | 通过 |
| V-CG13 | 分支目标处重新配置 | `v-ext.ll` `branch` | 通过 |
| V-CG14 | 调用后重新配置 | `v-ext.ll` `aftercall` | 通过 |
| V-CG15 | 5 位常量 `.vi`，大常量 `.vx` | `splat5/splat100` | 通过 |
| V-CG16 | 变量广播 `vmv.v.x` → `.vx` | `varbroadcast` | 通过 |
| V-CG17 | 归约返回标量 | `vmv.v.i`+`vredsum.vs`+`vmv.x.s` | 通过 |
| V-CG18 | 运行证据或缺口 | QEMU 8.2 / 6.2 | 通过（含缺口） |
| V-CG19 | O0 obj + `-verify-machineinstrs` 尺寸一致 | `v-ext.ll` 新增 RUN 行 | 通过 |

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
ninja -C "$B" llc llvm-mc llvm-objdump clang
```

静态 V 门禁：

```console
$ "$W/llvm/lib/Target/RVMin/scripts/rvmin-e2e-v.sh" "$B/bin"
PASS: V 1 program x 4 optimization levels = 4 objects
      every .text identical to its rvmin and riscv32 reassembly;
      .vv/.vi/.vx/reduction all selected
```

运行（QEMU 8.2）：

```console
$ "$W/llvm/lib/Target/RVMin/scripts/rvmin-run-v.sh" "$B/bin" \
    /tmp/qemu8/usr/bin/qemu-riscv32-static
rvmin: ok
```

运行（系统 QEMU 6.2，环境缺口）：

```console
$ "$W/llvm/lib/Target/RVMin/scripts/rvmin-run-v.sh" "$B/bin" \
    qemu-riscv32-static
SKIP: qemu-riscv32-static does not implement the V extension (SIGILL); use QEMU >= 8.2
$ echo $?
2
```

全量门禁：

```console
PASS 2/8: lit (CodeGen + MC)
PASS 3/8: encodings
PASS 4/8: disassembler
PASS 5/8: e2e (+ V subset)
PASS 6/8: 216 llc runs with -verify-machineinstrs
PASS 7/8: TableGen DSL contracts + status matrix + field effect
PASS 8/8: QEMU run (RV32I/M/A + F/D float/double + compressed demo)
ALL 8 GATES PASS
```

clang 矩阵：

```console
ALL CLANG MATRIX PASS: 432 objects, ELF32 EM_RISCV, -S/-c agree,
.text matches the riscv32 TargetInfo baseline
```

## 计数与口径

| 项 | 数值 | 说明 |
| --- | --- | --- |
| 编码语料 | 191 条 + 27 V | V 从 19 增到 27 |
| 反汇编 V | 27 条 | `.text` 逐字节往返 |
| V 静态 e2e | 4 对象 | O0–O3，各对照两个汇编器 |
| e2e 共享矩阵 | **288 对象** | 不变；V 不进入 `scripts/e2e/*.c` |
| clang 矩阵 | **432 对象** | 不变；`rv32imafdv` 已在其中 |
| llc 门禁 | **216 次** | 不变；`v-ext.ll` 新增 RUN 行不改变文件数 |
| 向量寄存器 | v0–v31 | `v4i32` 占一个 128 位寄存器 |
| VL/VTYPE | e32/m1/ta/ma | VL=4 |
| 运行 | QEMU 8.2 | 6.2/7.2 记录缺口 |

## 与完整 RVV 的差距

| 能力 | 本章 | 完整 RVV |
| --- | --- | --- |
| SEW | 32 | 8/16/32/64 |
| LMUL | 1 | 1/2/4/8，分数 LMUL |
| VLEN | 128 | 128–65536 |
| mask | 无（vm 固定 1） | v0 掩码、mask 逻辑指令 |
| 向量 FP | 无 | Zve32f/Zve64d 等 |
| 访存 | unit-stride | strided/indexed/segment |
| 归约 | `vredsum.vs` | 全表 `vred{min,max,and,or,xor}.vs` |
| 整数运算 | add/sub/mul/and/or/xor | 全表，含 widening/narrowing |
| 向量 ABI | 拒绝 | v8–v23 传参/返回、栈对齐 |
| vset 优化 | 块内缓存 | `RISCVInsertVSETVLI` 数据流 |

**对外口径**：RVMin 支持
`RV32IMAFD + C + Zicsr/Zifencei + Zve32x 子集`；
**不是** `rv32gcv` 全量支持。

## 常见坑

1. **不要把 V 语料塞进共享 `scripts/e2e/`。** 288 与 432 是第 45 章的验收
   基线；V 用独立脚本与独立目录。
2. **默认门禁不要依赖 QEMU 8。** 没有 V 的机器应看到明确的 SKIP/退出码 2，
   而不是整套门禁失败。
3. **O0 也要跑 obj + `-verify-machineinstrs`。** 只跑 asm 不会触发
   AsmPrinter 的尺寸校验，也就发现不了“报告 4、实际 8”。
4. **尺寸上界不是随便放大。** 只对可能插入 `vsetivli` 的数据向量指令报告
   8；`vset*` 仍是 4，标量路径完全不变。
5. **`-verify-machineinstrs` 看不到 AsmPrinter 插入的 vset。** 文本断言和
   反汇编仍是必需的。
6. **QEMU SIGILL 不等于编码错。** 先确认 QEMU 版本是否支持 V；编码正确性
   看对照 `riscv32 +v` 的字节门禁。
7. **`e2e-v` 的 `.vi/.vx` 断言从 O1 开始。** O0 仍把常量当完整向量；
   要求 O0 也出现 `.vi` 会得到与优化无关的失败。

## 练习

1. 把 `rvmin-run-v.sh` 接入 CI，但用 `QEMU` 环境变量指定 8.2；
   没有时保持退出码 2。
2. 给静态 V 门禁增加一个“每块 vset 数量”的上界断言，防止去重退化成
   每条都发。
3. 用 spike 复跑同一 `v-demo.elf`，比较输出。
4. 把 `v-demo.c` 扩展到 `vredmax.vs`（需要先补 MC 指令）。
5. 给 `getInstSizeInBytes` 增加一个 MIR 单元测试，直接断言
   `VLE32_V` 报告 8、`VSETIVLI` 报告 4。
