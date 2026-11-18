# 第 10 章：静态验收——8 项门禁与计数锁定

## 目标

第 00-09 章每章都有自己的 `check-chNN.sh`。这些脚本证明"本章的增量是
对的"，但不能证明"十章的增量放在一起还是对的"。本章不增加指令，而是
把静态后端固定成一套可重复执行的验收面：

1. `scripts/gpumin-verify-all.sh` 实现 8 项门禁；第 10 章要求
   1-5、8 全绿，6-7 明确 `SKIP`（模拟器第 12 章、调度第 18 章）；
2. `llvm/test/MC/GPUMin/` 与 `llvm/test/CodeGen/GPUMin/` 成为 lit
   语料，而不是只存在于 shell 脚本里；
3. `scripts/e2e/vecadd.ll` 走完 IR → `.s` → `.o`，并与重汇编的 `.o`
   逐字节一致；
4. `scripts/static-counts.txt` 锁定静态语料的数量（MC 文件、CodeGen
   文件、章节文档、门禁、e2e 程序、MC 指令数）；
5. 两个真实回归被门禁抓住并修掉：
   - `insertBranch` 把 `FBB` 当成"是否插入无条件分支"的条件，导致
     loop 的 backedge 在 block placement 后丢失；
   - `i1` 形式参数撞上 `CC_GPUMin` 只分配 `i32` 的边界（记录为已知
     限制，窄参数 ABI 留作练习）。

退出条件（可执行）：

```sh
llvm/lib/Target/GPUMin/scripts/gpumin-verify-all.sh <llvm-bin-dir>
```

输出必须是：

```text
PASS 1/8 ... PASS 5/8
SKIP 6/8: simulator e2e arrives with chapter 12
SKIP 7/8: scheduling model arrives with chapter 18
PASS 8/8 ...
Gates: 6 passed, 0 failed, 2 skipped
```

## 背景

### 为什么每章门禁还不够

每章的 `check-chNN.sh` 是**增量**测试：它只证明这一章新加的东西没坏。
但后端是一个整体，最常见的回归恰恰是"每章单独都对，合起来不对"：

- 第 05 章的分支编码是对的，第 07 章的调用也是对的，但
  `BranchFolder` 删除 backedge 之后，`MachineBlockPlacement` 没有把它
  加回来；
- 第 08 章的 i64 拆半是对的，第 07 章的 ABI 也是对的，但 `i1` 作为
  形式参数时 `CC_GPUMin` 没有对应规则，直接 `report_fatal_error`。

本章的 8 项门禁就是把这些"组合形状"固定下来。它不是替代章节测试，
而是叠加在章节测试之上的**总装检查**。

### 为什么要有计数锁

测试数量本身不是质量，但**数量突然下降**几乎总是质量问题：

- 有人删了一个 `*.ll`，`llvm-lit` 仍然全绿，因为少跑一个文件；
- 有人把 `check-ch09.sh` 改名，文档里的命令还在，但门禁没跑；
- 有人把 MC 语料从 38 条减到 20 条，编码覆盖率悄悄缩水。

`static-counts.txt` 把当前静态面锁成数字。增加覆盖时**故意**改这个
文件，并在 commit message 里说明；否则门禁失败。这和"不要过早承诺
数字"并不矛盾：第 10 章之前不锁，第 10 章之后锁的是**已经存在**的
语料，不是未来目标。

### 为什么 6、7 是 SKIP 而不是 FAIL

第 6 项（模拟器 e2e）依赖第 12 章，第 7 项（调度/occupancy）依赖
第 18 章。如果现在就让它们 FAIL，整个 harness 从第 10 章起永远是红
的，失去信号价值；如果删掉它们，又会掩盖"验收还没完成"。

所以 harness 从第一天就有 8 个槽位，但明确区分三种状态：

| 状态 | 含义 |
| --- | --- |
| `PASS` | 证据齐全，当前必须绿 |
| `SKIP` | 依赖章节还没到，明确不算通过 |
| `FAIL` | 依赖已存在但检查失败 |

MVP 的停止条件是 **8/8**，不是"6 个 PASS 也能收工"。

## 设计

### 8 项门禁

| # | 门禁 | 第 10 章实现 | 依赖 |
| --- | --- | --- | --- |
| 1 | target registry | `llc --version` 含 `gpumin`；`llvm-mc` 能汇编 | 第 00 章 |
| 2 | MC encoding vs 独立 encoder | `check-ch02.sh`（`tools/gpumin-encode.py`） | 第 02 章 |
| 3 | 反汇编往返 | `llvm-lit test/MC/GPUMin` | 第 02/10 章 |
| 4 | CodeGen lit | `llvm-lit test/CodeGen/GPUMin` | 第 03-09/10 章 |
| 5 | 静态 e2e（IR → object） | `e2e/vecadd.ll` + `.o` 对比 | 第 03-09 章 |
| 6 | 模拟器 e2e | `check-ch12.sh`（暂 SKIP） | 第 12 章 |
| 7 | 调度/occupancy | `check-ch18.sh`（暂 SKIP） | 第 18/19 章 |
| 8 | 文档/DSL/计数 | `static-counts.txt` + 文档检查 | 第 10 章 |

### lit 语料

第 10 章新增 7 个 CodeGen 文件：

```text
llvm/test/CodeGen/GPUMin/basic.ll
llvm/test/CodeGen/GPUMin/alu.ll
llvm/test/CodeGen/GPUMin/control.ll
llvm/test/CodeGen/GPUMin/memory.ll
llvm/test/CodeGen/GPUMin/calls.ll
llvm/test/CodeGen/GPUMin/types.ll
llvm/test/CodeGen/GPUMin/intrinsics.ll
```

和 3 个 MC 文件：

```text
llvm/test/MC/GPUMin/basic.s
llvm/test/MC/GPUMin/encoding.s
llvm/test/MC/GPUMin/special.s
```

它们不是把 shell 脚本原样搬过来，而是各自覆盖一个**可读的形状**：

| 文件 | 证明什么 |
| --- | --- |
| `basic.ll` | 返回值复制、常量物化、kernel `exit` |
| `alu.ll` | 八个 ALU funct、`setp`/`selp`、`addi` 折叠 |
| `control.ll` | if/else、loop backedge、predicated branch |
| `memory.ll` | `st/ld.global.b32`、11 位位移、索引左移 |
| `calls.ll` | `call`/`ret`、私有帧、lr 保存恢复、kernel `exit` |
| `types.ll` | 窄 load/store、i1 字节、i64 两半 |
| `intrinsics.ll` | 七个特殊寄存器读 |
| `basic.s` | MOV/MOVI/ALU/SETP/SELP 的字节级编码 |
| `encoding.s` | ADDI/内存/BRA/CALL/SR 的字节级编码 |
| `special.s` | 七个 SYS funct 的选择与反汇编 |

### 静态 e2e：`vecadd.ll`

`scripts/e2e/vecadd.ll` 是第一个端到端形状：kernel 读 `tid`，越界检查，
两次 global load、一次 add、一次 store：

```llvm
%t = call i32 @llvm.gpumin.tid()
%in = icmp ult i32 %t, %n
br i1 %in, label %body, label %done
...
%x = load i32, ptr addrspace(1) %pa
%y = load i32, ptr addrspace(1) %pb
%s = add i32 %x, %y
store i32 %s, ptr addrspace(1) %po
```

门禁检查：

- `.s` 里有 `sr.tid`、恰好两条 `ld.global.b32`、一条 `st.global.b32`、
  一条 `exit`；
- `llc -filetype=obj` 的 `.o` 与重汇编 `.s` 的 `.o` 的 `objdump -d -r`
  输出逐字节一致。

注意：这只是**静态** e2e。第 13 章之前 kernel 参数仍然走 `CC_GPUMin`
的 `r4-r11`；启动描述符要等第 13 章。静态门禁只证明"能编译、能链接、
编码一致"，不证明"跑得对"。

### 计数锁

`scripts/static-counts.txt`：

```text
mc_lit_files=3
codegen_lit_files=7
chapter_docs=10
chapter_gates=10
e2e_programs=1
mc_instructions=38
```

`mc_instructions` 的计数口径是：MC lit 文件里以助记符开头的行，排除
注释、RUN 行、标签和伪指令。当前 38 条覆盖 MOV/MOVI、8 条 ALU、
SETP/SELP、内存、分支、调用、特殊寄存器。

### 文档检查

门禁 8 还检查：

- `docs/00-*.md` 到 `docs/09-*.md` 都存在；
- `check-ch00.sh` 到 `check-ch09.sh` 都存在且可执行；
- 每篇章节文档都包含"退出条件"；
- 以上计数与锁文件一致。

这是"文档即契约"的最小版本：文档里写的命令必须真的存在，章节必须
真的有一个可执行退出条件。

## 实现

### harness 结构

`gpumin-verify-all.sh` 的顺序是有意的：

```text
1 registry
2 encoder
3 MC lit
4 CodeGen lit
5 static e2e
6 simulator (skip)
7 scheduling (skip)
8 docs/counts
```

前一项失败不会阻止后一项运行；最后汇总 `PASS/FAIL/SKIP`，有任何
`FAIL` 就返回非零。这样一次运行能看到所有问题，而不是修一个跑一次。

门禁 2 直接复用 `check-ch02.sh`，避免把独立编码器的逻辑复制第二份。
门禁 5 只用 `vecadd.ll`，不重复跑 03-09 的所有章节脚本；章节脚本由
各章自己的 tag/commit 保证，harness 负责总装。

### 回归 1：loop backedge 丢失

第 10 章第一次把 `loop` 放进 lit（带 `-verify-machineinstrs`），
O1/O2 立刻炸：

```text
*** Bad machine code: MBB has unexpected successors which are not branch
targets, fallthrough, EHPads, or inlineasm_br targets. ***
- basic block: %bb.2 body
```

机器码里 `bb.2.body` 有后继 `bb.1.head`，但块尾没有 `BRA`：

```text
bb.2.body:
  successors: %bb.1(0x80000000); %bb.1(100.00%)
  $r4 = ADD ...
  $r2 = ADDI ...
```

根因在 `GPUMinInstrInfo::insertBranch`：

```cpp
if (Cond.empty()) {
  if (FBB) {                    // ← 错：无条件分支看 TBB，不看 FBB
    BuildMI(... BRA).addMBB(TBB);
  }
  ...
}
```

`BranchFolder` 在某个布局下删掉了 `BRA`（当时它确实是 fallthrough），
`MachineBlockPlacement` 后来换了布局，需要把 `BRA` 加回来，于是调用
`insertBranch(MBB, TBB, /*FBB=*/nullptr, {}, DL)`。`FBB` 是 null，
旧代码什么都不插，CFG 后继就悬空了。修复：

```cpp
if (Cond.empty()) {
  if (TBB) {                    // 无条件分支：TBB 是唯一目标
    BuildMI(... BRA).addMBB(TBB);
  }
  ...
}
```

这个 bug 不影响第 05 章的直线 if/else，也不影响第 07 章的递归调用；
只有"带 backedge 的 loop + 后期 block placement + 验证器"三者同时
出现才暴露。这正是第 10 章存在的理由。

### 回归 2：`i1` 形式参数

把 `define void @st_i1(ptr, i1 %v)` 放进 `types.ll` 时：

```text
LLVM ERROR: unable to allocate function argument #1
```

`CC_GPUMin` 只有 `CCIfType<[i32], ...>`：`i1` 值在寄存器里是谓词，
但第 07 章的 ABI 只定义了 `i32` 走 `r4-r11`。本章不扩展 ABI，改为
从 `icmp` 产生谓词再 store：

```llvm
%c = icmp ne i32 %v, 0
store i1 %c, ptr addrspace(1) %p
```

并在文档里记下：**第 10 章不支持 `i1` 形式参数**。窄参数 ABI 是
第 08 章练习 4 的内容；在它落地之前，测试不应该假装它存在。

### 直接对源码树跑 lit

```sh
llvm-lit -v llvm/test/MC/GPUMin llvm/test/CodeGen/GPUMin
```

LLVM 的 `llvm/test/lit.cfg.py` 会从 `llvm-lit` 的位置找到构建目录的
工具（`llc`、`llvm-mc`、`FileCheck`），所以不需要先 `ninja check-llvm`，
也不需要把测试复制到构建树。门禁 3/4 用的就是这个方式。

### 计数检查的写法

```sh
mc_lit_files=$(find "$SRC/test/MC/GPUMin" -maxdepth 1 -name '*.s' | wc -l)
mc_instructions=$(cat "$SRC/test/MC/GPUMin"/*.s |
                  grep -cE '^[[:space:]]*[a-z][a-z0-9.]*')
```

`grep -cE` 只数助记符行，不会把 `# RUN:`、空行、`.L1:` 标签算进去。
计数失败时打印 `got X, want Y`，而不是只说"counts mismatch"。

## 验证

```sh
$ llvm/lib/Target/GPUMin/scripts/gpumin-verify-all.sh \
      /home/ubuntu/llvm/builds/pr-ctpop/bin
PASS 1/8: target registry ...
PASS 2/8: MC encoding matches tools/gpumin-encode.py byte for byte
PASS 3/8: MC lit: assemble, disassemble and re-encode
PASS 4/8: CodeGen lit: basic/alu/control/memory/calls/types/intrinsics
PASS 5/8: static e2e: vecadd IR -> object, and .o == reassembled .s
SKIP 6/8: simulator e2e arrives with chapter 12
SKIP 7/8: scheduling model arrives with chapter 18
PASS 8/8: docs/gates present and static counts locked

Gates: 6 passed, 0 failed, 2 skipped
```

单独跑 lit：

```text
$ llvm-lit -v llvm/test/MC/GPUMin llvm/test/CodeGen/GPUMin
-- Testing: 10 tests, 10 workers --
PASS: LLVM :: CodeGen/GPUMin/basic.ll
...
PASS: LLVM :: MC/GPUMin/special.s

Total Discovered Tests: 10
  Passed: 10 (100.00%)
```

## 常见坑

1. **`grep -L` 的退出码不能当"有没有缺失"用**。GNU grep 在
   `-L` 模式下即使没有任何文件被列出，也可能返回 0；本章第一版
   harness 就因此误报"有文档缺退出条件"。正确写法是捕获输出再判空。
2. **计数会把标签算进去**。`grep -c '^[a-z]'` 会把 `.L1:` 排除，但
   会把 `bra .L1` 算进去；口径要写进注释，否则下一个人不知道该数
   什么。
3. **锁文件不是目标**。增加测试时同步改 `static-counts.txt`；不要
   为了"让门禁绿"而删测试或调低数字。
4. **`-verify-machineinstrs` 对优化级别敏感**。O0 能过不代表 O1/O2
   能过；`control.ll` 的 loop 就是 O1/O2 才暴露。
5. **`i1` 形式参数还不在 ABI 里**。用 `icmp` 产生谓词，或者先做
   窄参数 ABI（第 08 章练习 4）。
6. **静态 e2e 不等于运行**。`vecadd.ll` 只证明 IR→ELF 和编码一致；
   它的结果对不对要等第 12 章的模拟器。
7. **改 ISA 要同时改独立编码器**。门禁 2 用的是 `tools/gpumin-encode.py`，
   它不共享 LLVM 代码；只改 TableGen 会让门禁 2 红。
8. **`objdump` 输出含 `file format` 行**。比较 `.o` 时要先过滤，
   否则路径/格式行会造成假 diff。
9. **lit 直接在源码树跑**。把 `build/test` 当参数会找不到测试文件；
   传 `llvm/test/...`。
10. **第 6/7 项 SKIP 不是通过**。MVP 完成前它们必须变成 PASS；
    不要把 `6 passed, 2 skipped` 当成 8/8。

## 练习

1. **嵌套循环**：给 `control.ll` 加一个双层 loop，跑
   `-verify-machineinstrs`；如果又触发 CFG 断言，用
   `-print-before/-print-after` 定位是哪个 pass 改了布局。
2. **优化级别矩阵**：把门禁 4 扩展成 `-O0/-O1/-O2` 三遍，统计每个
   文件在每个级别下的指令数；观察 `addi` 折叠和调度差异。
3. **静态 e2e 扩展**：把 SAXPY、reduction、matmul 的 IR 放进
   `scripts/e2e/`，只做 IR→ELF；它们会在第 15 章变成运行时程序。
4. **文档交叉检查**：让门禁 8 解析每篇文档里的 `check-chNN.sh` 引用，
   确认脚本存在且可执行。
5. **失败注入**：临时删掉一个 lit 文件，确认门禁 8 的计数检查会红；
   再恢复，确认它变绿。不要只相信"看起来对"。
6. **`i1` 参数 ABI**：在 `CC_GPUMin` 里给 `i1` 分配谓词寄存器，或在
   入口把它 zero-extend 到 `r4-r11`；为两种方案各写一个测试。
7. **空循环**：写 `define void @empty_loop(i32 %n)`，循环体只有
   `%i = add %i, 1`；这是 `insertBranch` 回归的最小复现，应该作为
   永久测试留下。

## 下一章

第 11 章：**SIMT 模型**。静态后端已经冻结：寄存器、编码、函数、内存、
类型、特殊寄存器都能从 IR 走到 ELF。下一章不再加指令，而是把
"lane / warp / active mask / uniform" 的语义写成模型文档，并让
`laneid`/`warpid` 的 lowering 与第 12 章模拟器的状态一一对应。第 12 章
会第一次真正执行这些程序，并把门禁 6 从 SKIP 变成 PASS。
