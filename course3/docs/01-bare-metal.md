# 第 01 章：裸机镜像与启动

## 目标

让 `rmach` 第一次真正“启动”一个裸机镜像：

- 内核用 clang 的标准 RISC-V 后端编译，链接到 `0x80000000`；
- `rmach` 复用第二课的 ELF 加载器，把 `PT_LOAD` 放进物理内存；
- 复位时 `pc = e_entry`、`sp = 0x80100000`；
- 能执行 RV32I+M 的 `_start`，`call kmain` 返回 7，`ebreak` 停机并回传 7。

这一章刻意**没有**特权级、CSR、中断和 MMU；先把“取指—译码—执行—停机”
这条链在物理地址上跑通。

## 背景：为什么从 0x80000000 开始

第二课的 `link.ld` 把用户程序放在 `0x10000`，因为 QEMU 用户态就是这么加载的
（`llvm/lib/Target/RVMin/scripts/rt/link.ld:5`）。裸机镜像没有加载器替它选地址：

| 约定 | 值 | 理由 |
| --- | --- | --- |
| 内核链接地址 | `0x80000000` | 与 QEMU `virt` 的 DRAM 起点一致，方便以后做设备冒烟 |
| 内核栈顶 | `0x80100000` | 内核镜像上方、DRAM 内部的 16 KiB 栈 |
| 复位入口 | `_start` | `ENTRY(_start)`；`rmach` 只信 `e_entry` |

内核用 clang 而不是 rvmin：特权指令、CSR、标准 ABI 都需要成熟后端。
rvmin 仍然负责用户程序——第三课要复用第二课 `demo`，那正是第一课的产物。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `course3/kernel/link.ld` | 内核布局，`ENTRY(_start)`，`. = 0x80000000` |
| `course3/kernel/start.S` | 设 `sp`、`call kmain`、`ebreak` |
| `course3/kernel/kmain.c` | `return 7`，用来证明调用链真的执行了 |
| `course3/mach/machine.{h,cpp}` | 寄存器、复位、取指/译码/执行、访存助手 |
| `course3/mach/trap.{h,cpp}` | `(cause, tval, epc)`；本章只用同步 cause |
| `course3/mach/main.cpp` | `--kernel`、`--dump-boot`、`--trace`、`--max-steps` |
| `course3/scripts/course3-verify.sh` | 3 项检查 |

复用（只读）：`course2/rsim/{decode,elf,memory}`。`decode` 的 `insn.def` 是
第一课验证过的唯一真源；`elf.cpp` 只接受 `ET_EXEC`，正好适合裸机镜像。

## 关键实现

### 1. 复位：只信 ELF，不猜

```cpp
void Machine::reset(const rsim::Image &image, rsim::Memory &memory) {
  memory_ = &memory;
  memory.load(image);
  memory.map(kKernelStackTop - kKernelStackSize, kKernelStackSize);
  for (uint32_t &reg : x) reg = 0;
  pc = image.entry;
  x[2] = kKernelStackTop;       // sp
  halted_ = false;
  executed_ = 0;
  exit_code_ = 0;
}
```

`memory.load(image)` 会把 `memsz > filesz` 的尾部保留为零页——第二课 ch02
的 `.bss` 规则在这里免费复用。

### 2. 取指与执行：所有访存都经过 Machine

```cpp
word = memory_->read32(at);          // 取指
const rsim::Decoded d = rsim::decode(word);
if (d.id == rsim::Insn::ILLEGAL) throw Trap(...);
++executed_;
```

`load8/16/32`、`store8/16/32` 六个助手把 `MemoryError` 转成带
`(cause, tval, epc)` 的 `Trap`，`epc` 是**出错指令**的地址。第 05 章会用总线
替换 `memory_`，第 06 章会在取指/访存前插入 Sv32 翻译——ALUs 的 switch 不用动。

### 3. `ebreak` 的临时语义

第 01 章还没有陷阱模型，所以：

```cpp
case rsim::Insn::ECALL:
case rsim::Insn::EBREAK:
  halted_ = true;
  exit_code_ = static_cast<int>(reg(10)); // a0
  break;
```

第 03 章会把它换成 `Breakpoint` / `EcallFromM` trap，并交给 `mtvec`。
**注意是 `reg(10)`（a0），不是 `d.rs1`**：`ebreak` 的 rs1 字段恒为 0。

### 4. `--dump-boot`

输出与 `llvm-readelf -hl` 可逐字段对照：

```text
entry 0x80000000
load vaddr=0x80000000 filesz=0x0000001c memsz=0x0000001c flags=0x5 align=0x1000
```

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B/course3"
"$W/course3/scripts/course3-verify.sh" "$B/bin" "$B/course3/rmach"
```

门禁 3 项（`3 checks`）：

1. `entry` 与 `PT_LOAD` 和 `llvm-readelf` 一致，且入口是 `0x80000000`；
2. 内核运行后退出码为 7，停机行是
   `halted at 0x80000010 after 6 instructions (exit 7)`；
3. `--trace` 第一行是 `T 80000000 ...`（复位向量本身）。

## 预期输出

```text
$ rmach --kernel kernel.elf --dump-boot
entry 0x80000000
load vaddr=0x80000000 filesz=0x0000001c memsz=0x0000001c flags=0x5 align=0x1000

$ rmach --kernel kernel.elf --trace
T 80000000 80100137 lui sp, 524544
T 80000004 00000097 auipc ra, 0
T 80000008 010080e7 jalr ra, 16(ra)
T 80000014 00700513 addi a0, zero, 7
T 80000018 00008067 jalr zero, 0(ra)
T 8000000c 00100073 ebreak
rmach: halted at 0x80000010 after 6 instructions (exit 7)
```

## 常见坑

1. **用 `d.rs1` 当退出码**。`ebreak` 的 `rs1` 恒为 0，第一次实现得到 `exit 0`；
   正确来源是 `a0 = reg(10)`。
2. **`clang -nostdlib` 调到了 GNU ld**。系统 `ld` 不认识 `elf32lriscv`，报
   `unrecognised emulation mode`；必须显式调用 `ld.lld`（或 `-fuse-ld=lld`）。
3. **门禁里的“预期非零退出”被 `set -e` 杀掉**。`rmach` 按设计返回 guest 的退出码
   （这里是 7），脚本必须 `set +e` 捕获后断言，不能直接调用。
4. **`--trace` 也会返回 7**。同样的 `set -e` 陷阱；trace 运行也要先捕获退出码。
5. **`li sp, 0x80100000` 不是一条指令**。汇编器会展开成 `lui` + `addi`，所以
   trace 的第一行是 `lui`，不是 `addi`——对拍 `llvm-objdump` 时不要按“源码行数”数。
6. **把 `0x80000000` 写成 `0x80000000` 之外的地址**。内核一取指就 `InstructionAccessFault`；
   先查 `readelf -h` 的 entry，再查链接脚本的 `. =`。

## 练习

1. 把内核栈改成 8 KiB，观察 `sp` 初值与栈顶对齐。
2. 给 `--dump-boot` 增加 symbols（复用 `Image::symbols`），打印 `_start` 与 `kmain` 的地址。
3. 让 `kmain` 返回 `argc` 风格的多个值，验证 `a0..a3` 在 `call/ret` 后保持不变。
4. 用 `llvm-objdump -d` 逐行核对 trace，找出伪指令展开的规律。
5. 把 `link.ld` 的基址改成 `0x80400000`，观察门禁里哪一条先红、为什么。
