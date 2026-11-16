# 第 02 章：CSR 文件与特权级

## 目标

把第 01 章“只有通用寄存器”的机器升级成有**控制状态**的机器：

- 一个按编号寻址的 CSR 文件（4096 项），而不是几个私有字段；
- 只实现课程用到的 15 个 CSR，每个都有明确的写入掩码；
- CSR 地址的 bits 9:8 决定特权：U 态访问任何非 0b00 的 CSR 都是非法指令；
- `--selftest` 用 22 条手算向量锁住掩码、只读位和对齐。

这一章还没有陷阱入口，也没有 `mret`；它先把“机器状态应该放在哪”定下来。

## 背景：为什么是 4096 项的数组

CSR 地址是 12 位，规范用 bits 9:8 编码访问权限：

| bits 9:8 | 含义 | 例子 |
| --- | --- | --- |
| `0b00` | 用户态可访问（只读计数器等） | `minstret` 的高位别名 |
| `0b01` | S 态 | `satp(0x180)` |
| `0b11` | M 态 | `mstatus(0x300)`、`mtvec(0x305)` |

用数组而不是 `switch` 存状态，可以让“编号 → 值”保持一一对应；`switch` 只负责
**写出哪些位允许变化**。课程实现：

| CSR | 地址 | 写入规则 |
| --- | --- | --- |
| `mstatus` | `0x300` | 只保留 MIE(3)、MPIE(7)、MPP(12:11) |
| `misa` | `0x301` | 固定报 `RV32 I M`，写忽略 |
| `mie` | `0x304` | 只保留 MTIE(7) |
| `mtvec` | `0x305` | base 4 字节对齐，mode 只允许 0/1 |
| `mscratch` | `0x340` | 全 32 位 |
| `mepc` | `0x341` | 清低 2 位 |
| `mcause` | `0x342` | 全 32 位（含中断位） |
| `mtval` | `0x343` | 全 32 位 |
| `mip` | `0x344` | MTIP 由 CLINT 拥有，软件写忽略 |
| `mhartid`/`mvendorid`/`marchid`/`mimpid` | `0xf14/0xf11..0xf13` | 只读，写 → 非法 |
| `minstret` | `0xb02` | 只读，返回已退休指令数 |
| `satp` | `0x180` | MODE/ASID/PPN，掩码 `0x803fffff` |

`M = 3`、`U = 0`；复位是 M。U 态访问 `satp` 也必须非法——它不是 M CSR，
但 bits 9:8 是 `0b01`，同样不是用户态能碰的。

## 代码地图

| 文件 | 作用 |
| --- | --- |
| `course3/mach/csr.h` | CSR 编号、掩码、`Priv`、`isPrivilegedCsr()` |
| `course3/mach/machine.{h,cpp}` | `csr_[4096]`、`readCsr/writeCsr`、CSR 指令执行 |
| `course3/mach/selftest.{h,cpp}` | 22 条手算向量 |
| `course3/mach/main.cpp` | `--selftest`、版本升到 0.2 |
| `course3/kernel/kmain.c` | 读 `mhartid/misa`，`mscratch` 往返 |
| `course3/scripts/course3-verify.sh` | 4 checks |

## 关键实现

### 1. 读：先查特权，再查编号

```cpp
uint32_t Machine::readCsr(uint16_t csr) const {
  if (priv_ == Priv::U && isPrivilegedCsr(csr))
    throw Trap(TrapCause::IllegalInstruction, csr, pc, "...");
  switch (csr) {
  case kCsrMstatus: return csr_[csr] & kMstatusMask;
  case kCsrMisa:    return (1u << 30) | (1u << 12) | (1u << 8); // RV32 I M
  ...
  default: throw Trap(TrapCause::IllegalInstruction, csr, pc, "unknown CSR");
  }
}
```

`misa` 的位：MXL(31:30)=1 表示 RV32，bit 8=I，bit 12=M。
`0x40000000 | 0x1000 | 0x100 = 0x40001100`。

### 2. 写：掩码是契约

```cpp
case kCsrMstatus: csr_[csr] = value & kMstatusMask; return;
case kCsrMepc:    csr_[csr] = value & ~3u;           return;
case kCsrMtvec:   csr_[csr] = (value & ~3u) | (value & 1u); return;
case kCsrMhartid: throw Trap(..., "read-only CSR write");
```

写 `0xffffffff` 到 `mstatus`，读回必须是 `0x1888`（MIE|MPIE|MPP）；
写 `0xffffffff` 到 `satp`，读回必须是 `0x803fffff`。

### 3. CSR 指令

```cpp
const uint32_t old = readCsr(d.csr);
const uint32_t src = isImmediateCsr(d.id) ? d.rs1 : a;
switch (d.id) {
case CSRRW:  writeCsr(d.csr, src); break;
case CSRRS:  if (src != 0) writeCsr(d.csr, old | src); break;
default:     if (src != 0) writeCsr(d.csr, old & ~src); break;
}
setReg(d.rd, old);
```

`CSRRS/CSRRC` 的 `rs1 = x0` 表示“只读不写”，这是 `csrr` 别名的语义。

### 4. trace 里的 CSR 名字

第二课的 `format()` 对未知 CSR 打印 `"0x" + std::to_string(csr)`，
于是 `mhartid(0xf14)` 变成 `0x3860`（十进制 3860 前面加 0x）。
第三课在 `machine.cpp` 里自带 `formatForTrace()`，已知 CSR 用名字，
其余用 `%03x`，所以 trace 是 `csrrs a0, mhartid, zero`。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

ninja -C "$B/course3"
"$B/course3/rmach" --selftest
"$W/course3/scripts/course3-verify.sh" "$B/bin" "$B/course3/rmach"
```

门禁 4 项（`4 checks`）：

1. 内核 ELF 的 entry/PT_LOAD 与 `llvm-readelf` 一致；
2. 内核读 `mhartid/misa`、`mscratch` 往返后退出 0；
3. trace 第一行是复位向量；
4. `--selftest` 输出 `22 checks, 0 failures`。

## 预期输出

```text
$ rmach --selftest
selftest: 22 checks, 0 failures

$ rmach --kernel kernel.elf --trace
T 80000014 f1402573 csrrs a0, mhartid, zero
T 80000024 ...
T 80000028 301025f3 csrrs a1, misa, zero
T 80000040 34051073 csrrw zero, mscratch, a0
rmach: halted at 0x80000010 after 20 instructions (exit 0)
```

## 常见坑

1. **只把 `0b11` 当“特权 CSR”**。`satp` 是 `0b01`（S 态），U 态一样不能访问；
   正确判据是 `bits 9:8 != 0`。第一次实现漏掉了这条，selftest 立刻红。
2. **`misa` 手算错位**。`0x40000000 | 0x1000 | 0x100` 是 `0x40001100`，
   不是 `0x40101100`（把 bit 8 算成了 bit 11）。
3. **把 `mip` 当普通可写寄存器**。MTIP 由 CLINT 置位、由 `mtimecmp` 清除；
   软件写 `mip` 必须被忽略，否则第 04 章的中断会“永远挂起”。
4. **`mepc/mtvec` 不掩低位**。`mepc` 低 2 位恒为 0，`mtvec` 的 mode 只允许 0/1；
   不掩会让第 03 章的 `mret` 跳到奇数地址。
5. **直接信第二课的 `format()`**。未知 CSR 会打印成 `0x3860`；trace 必须自己格式化。
6. **以为 `minstret` 可写**。本章把它当只读计数器；写忽略，读返回 `executed_`。

## 练习

1. 给 `mstatus` 加 `MPRV(17)`，并写一条 selftest 验证掩码。
2. 实现 `misa` 的 WARL 写入：允许清掉 M 位，观察后续 `mul` 是否变成非法指令。
3. 增加 `mcountinhibit(0x320)`，让 `minstret` 可以停止计数。
4. 把 `csr_` 从 `uint32_t[4096]` 改成稀疏表，比较 `sizeof(Machine)` 与访问开销。
5. 给 trace 增加 `--trace-csr`，每条 CSR 指令后打印新旧值。
