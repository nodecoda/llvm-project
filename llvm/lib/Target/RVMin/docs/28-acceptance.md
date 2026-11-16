# 第 28 章：clang 一等 target 验收

## 目标

第三篇（24–27 章）把 `rvmin` 从“能被 `llc -march=rvmin` 按名字选中”变成
“`clang --target=rvmin-unknown-elf` 一条命令能编译、汇编、链接、运行”。
本章不加新功能，只做两件事：

1. 用 **80 个对象**的矩阵把“driver 路径”和“TargetInfo 不改代码生成”这两条
   承诺变成可执行断言；
2. 把 AC1–AC10 收成一张能逐条复现的验收表，并写清没做的部分。

新增门禁脚本：

```text
scripts/rvmin-clang-matrix.sh
```

## 背景：计划里的口径，被实测推翻了一半

`.omx/plans/2026-11-16-rvmin-clang-target.md` §7 原写的是：

> 每个对象的 `.text` 与 riscv32+llc 基线逐字节相同。

真跑起来发现这条口径**不可判定**，原因有三层，按发现顺序：

1. **`llc` 不读 IR 里的 `target-features` 属性。**
   clang driver 把 `-march=rv32im` 翻译成 `-target-feature +m`，这个字符串会
   写进函数的 `target-features` 属性，同时也会传给 clang 内部的
   `TargetMachine`。但 `llc` 只从命令行 `-mattr` 构建 subtarget。
   于是“同一个 IR 直接喂 llc”会丢掉 `+m`：`mul` 从单条 `mul` 退回
   `__mulsi3` 调用，`.text` 1040 vs 772 字节。
2. **`clang -c` 与 `llc` 对同一份 IR 的指令选择可以不同。**
   最典型的是 `trace()` 的循环归纳变量：一边用 `i: 0..6 step 1`，
   另一边用 `byte: 0..144 step 24`，两者语义相同但字节不同。
   这不是 RVMin 的问题——把 clang 打印出来的 `.ll` 再喂回 `clang -cc1`
   也一样，说明 clang 后端拿到的模块里有文本 IR 表达不了的信息。
3. **`-O2/-O3` 的向量化成本模型依赖 TargetMachine。**
   `memory.c` 的 `strlen_like` 在 rvmin 下被 SLP 组成了归约树
   （`op.rdx*`），在 riscv32 下保持线性累加。这是 TTI 策略差异，
   属于“合法但不同”，不是 ABI 问题。

所以本章把差分拆成两条互不重叠的断言：

| 断言 | 比什么 | 证明了什么 |
| --- | --- | --- |
| **汇编器一致性** | `clang -c a.c` 的 `.text` vs `clang -S a.c` 再 `clang -c a.s` 的 `.text` | `-S`/`-c` 两种输出路径一致，集成汇编器忠实 |
| **TargetInfo 平价** | rvmin 的 IR → `llc` vs riscv32 的 IR → **同一个** `llc` | 两个 TargetInfo 产出的 ABI/属性没有改变代码生成 |

第二条的两个细节写进了脚本注释，也值得在这里重复：

- 基线必须显式 `-mattr`，否则比的是 RV32I 与 RV32IM 两种构建；
- 差分时两边都加 `-fno-slp-vectorize -fno-vectorize`。关掉矢量器后
  80/80 全绿；开着时只有 `memory.c -O2/-O3` 的 4 格不同，原因就是上面第 3 条。
  换句话说：**这个门禁测的是 ABI，不是优化策略**。

## 代码地图

| 文件 | 改动 |
| --- | --- |
| `scripts/rvmin-clang-matrix.sh` | 新脚本：80 格 × 3 项检查 + ISA 标记 |
| `clang/lib/Driver/ToolChains/Clang.cpp` | `isSignedCharDefault()` 增加 `Triple::rvmin`（见“常见坑 1”） |
| `clang/test/Driver/rvmin-target.c` | 新增 `-fno-signed-char` 的 FileCheck 回归 |
| `docs/28-acceptance.md` | 本章 |
| `docs/README.md`、`README.md` | 章节表补第 28 行 |

矩阵脚本的结构：

```text
for march in rv32i rv32im:                 # 2
  ISA 标记检查（arith.c 里有没有 mul）
  for opt in O0 O1 O2 O3:                  # 4
    for src in scripts/e2e/*.c:            # 10
      1. clang --target=rvmin -c      -> 空 stderr + ELF32 + EM_RISCV
      2. clang --target=rvmin -S | -c -> .text 与第 1 步相同
      3. rvmin IR -> llc
         riscv32 IR -> llc
         （两边 -mattr 相同、都关矢量器） -> .text 逐字节相同
```

## 关键实现

### 1. ISA 标记：`mul` 必须出现/不出现

`e2e/arith.c` 里有乘法。`-march=rv32im` 的对象必须含 `mul` 指令，
`-march=rv32i` 的对象必须不含。检查时不只看对象，还要给反汇编器
`--mattr=+m`：

```bash
"$BIN/llvm-objdump" -d --mattr=+m isa-rv32im.o
```

因为 RVMin **不发射 `.riscv.attributes`**，`llvm-objdump` 无从得知扩展；
不加 `--mattr` 时 `mul` 会显示成 `<unknown>`。这是第三篇结束时仍存在的
一个可见限制（见“明确未做”）。

### 2. 汇编器一致性

```bash
clang --target=rvmin -march=rv32im -O2 -S a.c -o a.s
clang --target=rvmin -march=rv32im -c a.s -o a.asm.o   # 注意：不给 -O
clang --target=rvmin -march=rv32im -O2 -c a.c -o a.o
llvm-objcopy --dump-section .text=... a.asm.o a.o
cmp ...
```

`-S` 与 `-c` 走同一套 codegen，所以 `.text` 必须相同；不同就说明
集成汇编器丢了信息（重定位、对齐、伪指令展开）。

### 3. TargetInfo 平价

```bash
clang --target=rvmin-unknown-elf   -march=rv32im -mabi=ilp32 -O2 \
  -ffreestanding -fno-builtin -fno-slp-vectorize -fno-vectorize \
  -S -emit-llvm a.c -o a.rvmin.ll
clang --target=riscv32-unknown-elf -march=rv32im -mabi=ilp32 -O2 \
  -ffreestanding -fno-builtin -fno-slp-vectorize -fno-vectorize \
  -S -emit-llvm a.c -o a.riscv.ll

llc -march=rvmin -mtriple=rvmin-unknown-elf -O2 -mattr=+m \
  -filetype=obj a.rvmin.ll -o a.rvmin.o
llc -march=rvmin -mtriple=rvmin-unknown-elf -O2 -mattr=+m \
  -filetype=obj a.riscv.ll -o a.riscv.o

llvm-objcopy --dump-section .text=... a.rvmin.o a.riscv.o
cmp ...
```

两边进的是**同一个后端、同一组 feature**，唯一的变量是 clang 用哪个
TargetInfo 生成 IR。任何 `.text` 差异都只能来自 ABI/属性/builtin 的差别。

## 验收表（AC1–AC10）

| ID | 标准 | 复现命令 | 当前证据 |
| --- | --- | --- | --- |
| AC1 | triple/arch 可解析 | `TargetParserTests --gtest_filter=Triple.RVMin` | 1/1 通过；`llc -mtriple=rvmin-unknown-elf` 可用 |
| AC2 | 端序/位宽/格式 | 同上（`Triple.RVMin` 断言 LE/32/ELF/data layout） | 通过 |
| AC3 | IR 可用 | `clang --target=rvmin -S -emit-llvm` | triple `rvmin-unknown-unknown-elf`，`target-abi=ilp32` |
| AC4 | object 可用 | `rvmin-clang-matrix.sh` 第 1 项 | 80/80 `ELF32` + `Machine: RISC-V`，stderr 全空 |
| AC5 | 集成汇编器 | 矩阵第 2 项 | 80/80 `-S` 与 `-c` 的 `.text` 相同 |
| AC6 | feature 正确 | `clang/test/Driver/rvmin-target.c` + 矩阵 ISA 标记 | `rv32i` 无 `+m/+a/+c`，`rv32im` 有 `+m`；对象里 `mul` 只在 `rv32im` 出现 |
| AC7 | 负例诊断 | `clang/test/Driver/rvmin-invalid.c` | rv64 / rv32ic / rv32if / lp64 / `-fPIC` / `-mcmodel=large` 全部非零退出 |
| AC8 | 链接 | `rvmin-clang-run.sh` | link 行含 `-m elf32lriscv`，产物 `llvm-readelf -h` 为 EM_RISCV |
| AC9 | 运行 | `rvmin-clang-run.sh`、`rvmin-run.sh` | 6 组 clang 驱动程序 + 9 次 QEMU 运行，退出码 0 |
| AC10 | 不回归 | `rvmin-verify-all.sh` + 矩阵第 3 项 | 6/6；lit 24/24；144 条编码；117+6 反汇编；128 次机器指令校验；80/80 TargetInfo 平价 |

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

cd "$B" && ninja clang llc llvm-mc llvm-objdump llvm-readelf llvm-objcopy \
  FileCheck llvm-lit TargetParserTests

# 1. 后端既有门禁
"$W/llvm/lib/Target/RVMin/scripts/rvmin-verify-all.sh" "$B/bin"

# 2. 链接 + 运行（两条路径）
"$W/llvm/lib/Target/RVMin/scripts/rvmin-run.sh"       "$B/bin"
"$W/llvm/lib/Target/RVMin/scripts/rvmin-clang-run.sh" "$B/bin"

# 3. 本章矩阵
"$W/llvm/lib/Target/RVMin/scripts/rvmin-clang-matrix.sh" "$B/bin"

# 4. clang lit 与 Triple 单测
"$B/bin/llvm-lit" -s "$W/clang/test/Driver/rvmin-target.c" \
                     "$W/clang/test/Driver/rvmin-invalid.c" \
                     "$W/clang/test/Preprocessor/rvmin-target.c" \
                     "$W/clang/test/CodeGen/rvmin-abi.c"
"$B/unittests/TargetParser/TargetParserTests" --gtest_filter=Triple.RVMin
```

## 预期输出

```text
PASS: e2e corpus under -march=rv32i -O0
...
PASS: e2e corpus under -march=rv32im -O3
ALL CLANG MATRIX PASS: 80 objects, ELF32 EM_RISCV, -S/-c agree,
                       .text matches the riscv32 TargetInfo baseline
```

矩阵在本地约 31 秒（80 格 × 7 次工具调用）。

## 数字快照（`rvmin-ch28` 之后）

| 项 | 数量 |
| --- | --- |
| 章 tag | 29 个（`rvmin-ch00`…`rvmin-ch28`） |
| RVMin lit | 24 个（CodeGen 13 + MC 11） |
| clang lit | 4 个（Driver 2 + Preprocessor 1 + CodeGen 1） |
| 编码语料 | 85 RV32I + 12 M + 19 Zbb + 8 Zbs + 20 A = 144 条 |
| 反汇编往返 | 117 条 + 6 条标签分支 |
| 后端 e2e | 10 程序 × 4 优化级 × 2 特性集 = 80 对象，45484 字节 |
| clang 矩阵 | 10 程序 × 4 优化级 × 2 ISA = 80 对象 × 3 项检查 |
| `-verify-machineinstrs` | 128 次 `llc` |
| QEMU 运行 | 9（逐层路径）+ 6 组（clang 驱动路径） |

## 明确未做（第三篇新增部分）

| 方向 | 现状 | 原因 / 影响 |
| --- | --- | --- |
| `.riscv.attributes` 发射 | 不做 | `llvm-objdump`/`readelf -A` 看不到扩展；反汇编要显式 `--mattr` |
| 每个函数独立 subtarget | 不做 | `RVMinTargetMachine::getSubtargetImpl` 返回单一 subtarget，忽略函数 `target-features`；`clang` 走 TargetMachine 传 feature 所以正确，`llc` 必须显式 `-mattr` |
| `.addrsig` | 由 clang 发射，llc 路径不发射 | 只影响 `llvm_addrsig` 段，不参与执行；矩阵只比 `.text` |
| libc / 头文件 | 不做 | 仍用 freestanding + 自写 runtime；clang 的 resource dir 里没有 rvmin 的 sysroot |
| rv64 / C / F / D / V / PIC / 大代码模型 | 诊断拒绝 | 与第一课合同一致，见 `rvmin-invalid.c` |
| 多文件 LTO | 不做 | 没有验证 `-flto` 下的 triple/feature 合并；单文件已覆盖 |

## 常见坑（本章真实踩到的）

1. **`char` 是有符号的，RISC-V ABI 要求无符号。**
   矩阵第一次跑就红了：`memory.c` 的 `mixed_fields` 在 rvmin 下生成
   `sext i8`，riscv32 下是 `zext i8`，`.text` 里 `lb` vs `lbu` 差一个字节。
   根因在 driver 的 `isSignedCharDefault()`——它把 `riscv32/riscv64` 列进
   “默认无符号”，但 `rvmin` 是新 arch，漏了。修法是一行 `case`，
   回归是 `clang/test/Driver/rvmin-target.c` 里的 `-fno-signed-char` 断言。
   **这个 bug 只有跑矩阵才会暴露**，单看 `-march=rv32im` 的对象完全正常。
2. **`llc` 不读 `target-features` 属性。**
   不要用“clang 出 IR → llc”去验证 driver 的 feature 传递；要么显式
   `-mattr`，要么直接看 `clang -###`。本章两条都做了。
3. **`-S -emit-llvm` 的文本 IR 不等于 clang 后端拿到的模块。**
   同一份 `.ll` 喂 `clang -cc1` 与喂 `llc` 会得到不同的 LSR 结果；
   所以“拿 clang 的 IR 再喂 llc 应该逐字节相同”这个直觉是错的。
   矩阵因此改用“两条 IR 都过同一个 llc”的口径。
4. **给汇编输入传 `-O2` 会有 unused 警告。**
   `clang -c a.s -O2` 会打印 `argument unused during compilation: '-O2'`，
   把脚本的“stderr 必须为空”判据弄红。汇编步骤不要传 `-O`。
5. **反汇编 `mul` 得到 `<unknown>` 不是后端坏了。**
   RVMin 没有 `.riscv.attributes`，`llvm-objdump` 默认只按 RV32I 译码；
   加 `--mattr=+m` 即可。第 20 章的往返门禁一直显式带 `-mattr`。

## 练习

1. 把矩阵的矢量器开关做成参数（`--default-pipeline`），记录
   `memory.c -O2/-O3` 的 4 格差异，并用 `opt -passes=print<slp-vectorizer>`
   解释归约树为什么不同。
2. 给 RVMin 发射最小的 `.riscv.attributes`（只写 `Tag_RISCV_arch`），
   然后删掉矩阵里的 `--mattr=+m`，确认 `llvm-objdump` 能自己认出 `mul`。
3. 让 `RVMinTargetMachine::getSubtargetImpl(F)` 读取函数
   `target-features`，去掉矩阵里的 `-mattr`，验证 80 格仍然全绿；
   评估这会怎样影响第 13 章的 feature 门禁。
4. 把 AC1–AC10 写成一个 `check-part3-ac.sh`，让每条“复现命令”都成为断言。
