# 第 08 章：陷阱模型与 ebreak

## 目标

把第 04 章的 `(cause, tval, epc)` 变成**机器状态**，并让程序能自己处理错误：

- 实现最小 CSR 集：`mtvec`、`mepc`、`mcause`、`mtval`（外加只读的 `minstret`）；
- 实现 Zicsr 六条指令 + `mret`；
- trap 发生时写 `mepc/mcause/mtval`；`mtvec != 0` 就跳到处理程序，否则停住；
- `--dump-csr` 在运行（包括 trap）之后打印这五个寄存器。

本章结束时，“非法指令”不再只是一句诊断：程序自己就能看见 `mcause=2`。

## 背景：为什么需要 CSR

第 04 章已经能精确指出“哪条指令、什么原因、涉及哪个地址”，但这些信息只存在于
宿主的 stderr 里。真正的处理器把它们写进 CSR，让**软件**决定怎么办：

| CSR | 内容 | 本章的实现 |
| --- | --- | --- |
| `mtvec` | trap 入口 | 只支持直接模式（`mode = 0`），写入时清低两位 |
| `mepc` | 出错指令地址 | trap 时来自 `Trap::epc`，写入时清低两位 |
| `mcause` | 原因编号 | 直接用 `TrapCause` 的数值 |
| `mtval` | 附加信息 | 非法指令=指令字；访存=有效地址；ebreak=指令地址 |
| `minstret` | 已退休指令数 | 就是 `executed_`，只读 |

QEMU 用户态**不**模拟这些（它们属于机器态），所以本章没有 QEMU 差分：
实测 `mtvec` 在 QEMU 用户态里没有意义，非法指令直接 `SIGILL`。
这也正好说明“模拟器 ≠ 用户态加载器”。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `rsim/insn.def` | 新增 `CSRRW/CSRRS/CSRRC`、立即数形式、`MRET` |
| `rsim/decode.cpp` | `Fmt::Csr`、`csrName()`、`isImmediateCsr()` |
| `rsim/cpu.cpp` | `readCsr/writeCsr`、`step()` 的 trap 包装、`executeOne()` |
| `rsim/main.cpp` | `--dump-csr` |
| `tests/illegal.s` / `ebreak.s` / `trap-handler.s` | 三个取证程序 |

## 关键实现

### 1. `step()` 分成两层

```cpp
bool Cpu::step() {
  if (halted_) return false;
  try { return executeOne(); }
  catch (const Trap &trap) {
    mepc_ = trap.epc; mcause_ = (uint32_t)trap.cause; mtval_ = trap.tval;
    if (mtvec_ == 0) throw;      // 没有处理程序：交给宿主
    pc = mtvec_ & ~3u;           // 有：进入处理程序
    return true;
  }
}
```

`executeOne()` 保持“一条指令”的纯粹语义；陷入逻辑只有这一处 7 行。
`throw;` 必须在 catch 里，不能抽成独立函数——这是 C++ 的约束，也顺便保证了
“没有 handler 时语义和第 04 章完全一致”。

### 2. Zicsr 的读写语义

| 指令 | 行为 |
| --- | --- |
| `csrrw rd, csr, rs1` | `rd = csr; csr = rs1` |
| `csrrs` | `rd = csr; csr |= rs1`（`rs1=x0` 只读，`csrr` 别名） |
| `csrrc` | `rd = csr; csr &= ~rs1` |
| `csrrwi/rs/rc` | 同上，源是 5 位立即数（沿用 `rs1` 字段） |

未知 CSR 一律当作非法指令（`mtval = CSR 号`），而不是静默返回 0——
“读到一个不存在的寄存器”会让错误跑到很远的地方才爆。

### 3. `mret` 与“处理程序必须自己前进”

`mret` 只做一件事：`pc = mepc`。如果处理程序忘了把 `mepc` 加 4，
就会在出错指令上反复 trap，看起来像死循环。`tests/trap-handler.s` 正是把这个
步骤写在明面上：

```asm
.Lh_resume:
        addi    t2, t2, 4
        csrrw   zero, mepc, t2
        mret
```

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop
"$W/course2/scripts/course2-verify.sh" "$B/bin" "$B/course2/rsim"
```

第 08 章新增 3 项检查（总数 132 → 135）：

1. `tests/illegal.s`：退出 126，`--dump-csr` 里 `mcause=2`，`mepc` 等于
   `readelf` 给出的 `main` 地址；
2. `tests/ebreak.s`：退出 126，`mcause=3`，`mtval == mepc`；
3. `tests/trap-handler.s`：安装 `mtvec` 后连续处理“非法指令 + `ebreak`”，
   逐项校验 `mcause/mepc/mtval`，`mret` 回到原处，最终退出 0。

三份测试程序用 **clang 的 RISC-V 汇编器**（`-march=rv32i_zicsr`）编译，
因为第一课的 RVMin 汇编器没有 Zicsr；编码是标准的，第 03 章已经证明
我们的译码器与参考编码逐字一致。

## 预期输出

```text
$ rsim --run illegal --dump-csr; echo $?
rsim: trap: illegal instruction: tval=0x00000000 epc=0x00010048 (no RV32I/M instruction matches)
mtvec    0x00000000
mepc     0x00010048
mcause   0x00000002
mtval    0x00000000
minstret 3
126

$ rsim --run trap_handler --dump-csr; echo $?
rvmin: ok
mtvec    0x0001007c
mepc     0x00010068
mcause   0x00000003
mtval    0x00010064
minstret 57
0
```

`trap_handler` 结束时 `mcause=3` 是因为最后一次 trap 是 `ebreak`；
`mepc` 已经是“+4 之后”的值，所以处理程序才能返回。

## 常见坑

1. **拿 `llvm-mc -triple=rvmin` 汇编 `csrrw`**。第一课没实现 Zicsr，直接报错。
   本章改用 `clang --target=riscv32-unknown-elf -march=rv32i_zicsr`。
2. **clang 默认开链接松弛**。`ld.lld` 会报
   `relocation R_RISCV_ALIGN requires unimplemented linker relaxation`；
   加 `-mno-relax` 即可。
3. **`match_imm` 塞不下 `0x302`**。它原本是 `uint8_t`，`mret` 的匹配值
   `0x302` 直接触发 `-Wnarrowing`；扩成 `uint16_t`，通配值仍是 `0xff`。
4. **处理程序忘了推进 `mepc`**。症状是无限 trap，日志里全是同一条指令。
5. **想用 QEMU 做本章差分**。用户态 QEMU 没有 `mtvec/mret` 语义，非法指令
   会变成 `SIGILL`；本章的裁判是 `--dump-csr` 与自带的处理程序。
6. **门禁里把命令替换写成算术展开**。`MAIN=$(("$READELF" ...))` 会被当成
   `$(( ))`，报 `$8: unbound variable`；正确写法是 `MAIN=$(...)`。

## 练习

1. 给 `mcause` 增加“中断位”（最高位），为将来加定时器中断做准备。
2. 实现 `mtvec` 的向量模式（`mode = 1`），让不同 cause 跳到不同的表项。
3. 让 `--dump-csr` 支持 `--dump-csr=<name>`，只打印一个寄存器。
4. 写一个处理程序，把 `mcause` 转成可读字符串并 `write` 到 stdout。
