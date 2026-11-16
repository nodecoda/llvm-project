# 第 01 章：ELF32 加载器

## 目标

把第一课 `ld.lld` 产出的静态 ELF 读进内存，并证明读得对：

1. 只接受 `ELFCLASS32` + `ELFDATA2LSB` + `e_machine = 243` + `ET_EXEC`；
2. 遍历程序头，只映射 `PT_LOAD`，拒绝 `PT_INTERP`/`PT_DYNAMIC`；
3. 入口点必须落在某个 `PT_LOAD` 内；
4. `--dump-elf` 的入口与段表与 `llvm-readelf -hl` **逐项一致**；
5. 截断、坏 magic、`ET_REL`、文件不存在都返回 125 并给一行诊断。

## 背景：链接器给了我们什么

第一课的 `scripts/rt/link.ld` 把 `start.S`、程序、运行时链接成：

```text
Type: EXEC   Machine: RISC-V   Entry: 0x10000
Program Headers: 4
  LOAD 0x001000 0x00010000 ... R E 0x1000   # .text
  LOAD 0x0014f4 0x000104f4 ... R   0x1000   # .rodata
  LOAD 0x001518 0x00010518 ... RW  0x1000   # .data / .bss
  GNU_STACK ...
```

模拟器只需要其中三样东西：**入口地址、每个 `PT_LOAD` 的虚拟地址、文件数据**。
其余（节头、符号表、重定位）在静态可执行文件里都已经解析完毕，加载时可以忽略。

ELF32 头是固定 52 字节，关键字段的偏移如下（小端）：

| 偏移 | 大小 | 字段 | 本章的检查 |
| --- | --- | --- | --- |
| 0 | 16 | `e_ident` | magic、class、data、version |
| 16 | 2 | `e_type` | 必须是 `ET_EXEC`(2) |
| 18 | 2 | `e_machine` | 必须是 `EM_RISCV`(243) |
| 20 | 4 | `e_version` | 必须是 1 |
| 24 | 4 | `e_entry` | 必须落在某个 `PT_LOAD` 内 |
| 28 | 4 | `e_phoff` | 程序头表偏移 |
| 42 | 2 | `e_phentsize` | 至少 32 |
| 44 | 2 | `e_phnum` | 至少 1 |

每个程序头 32 字节，本章用到的字段：

| 偏移 | 字段 | 用途 |
| --- | --- | --- |
| 0 | `p_type` | `PT_LOAD`(1) 才映射 |
| 4 | `p_offset` | 文件内偏移 |
| 8 | `p_vaddr` | 虚拟地址 |
| 16 | `p_filesz` | 文件里有几个字节 |
| 20 | `p_memsz` | 内存里占几个字节（多出来的清零） |
| 24 | `p_flags` | `X=1, W=2, R=4` |
| 28 | `p_align` | 必须是 2 的幂 |

## 代码地图

| 位置 | 职责 |
| --- | --- |
| `rsim/elf.h` | `Image`/`Segment`/`ElfError` 与 `loadElf` 接口 |
| `rsim/elf.cpp` | 头部校验、程序头遍历、文件数据拷贝、错误诊断 |
| `rsim/memory.h` | 稀疏页表与 `load(Image)` |
| `rsim/memory.cpp` | 按页分配、`memsz > filesz` 的零填充、读写 |
| `rsim/main.cpp` | `--dump-elf` 与“只加载”两种模式 |
| `scripts/course2-verify.sh` | 5 个可执行文件对拍 + 4 种拒绝路径 |
| `docs/01-elf-loader.md` | 本章 |

## 关键实现

### 1. 先做算术，再信任文件

所有“偏移 + 长度”都用 `uint64_t` 算，避免 32 位回绕：

```cpp
const uint64_t ph_end = uint64_t(phoff) + uint64_t(phnum) * phentsize;
if (ph_end > buf.size())
  throw ElfError("program header table is truncated");
```

段数据同理：

```cpp
if (uint64_t(offset) + filesz > buf.size())
  throw ElfError("PT_LOAD file range is truncated");
if (uint64_t(vaddr) + memsz > (uint64_t{1} << 32))
  throw ElfError("PT_LOAD virtual range overflows 32 bits");
```

### 2. 只映射 `PT_LOAD`

```cpp
if (p_type == PT_INTERP)  throw ElfError("PT_INTERP is not supported ...");
if (p_type == PT_DYNAMIC) throw ElfError("PT_DYNAMIC is not supported ...");
if (p_type == PT_NULL || p_type != PT_LOAD) continue;
```

`GNU_STACK` 之类直接跳过。这样 `ET_EXEC` 的输入会被完整建模，
而动态链接产物在加载阶段就失败，而不是跑到一半才崩。

### 3. 稀疏内存：4 KiB 一页

```cpp
std::unordered_map<uint32_t, std::array<uint8_t, 4096>> pages_;
```

`map(addr, size)` 只分配覆盖到的页；`read/write` 找不到页就抛
`MemoryError("unmapped access at 0x........")`。这样：

- 4 GiB 地址空间不花钱；
- 越界访问立刻暴露，模拟器不会凭空造零；
- `.bss` 的 `memsz > filesz` 在 `load()` 里天然是零（页初始化为 0）。

### 4. `--dump-elf` 的输出就是契约

```text
entry 0x00010000
load vaddr=0x00010000 filesz=0x000004f4 memsz=0x000004f4 flags=0x5 align=0x1000
```

`flags=0x5` 是 `R|X`。验证脚本用 Python 把 `llvm-readelf -l` 的
`R E`/`RW ` 三字符列换算成同一组位，再逐段比较。这样门禁不会依赖
“手写 golden 刚好和实现一致”。

## 构建与验证

```bash
W=/home/ubuntu/llvm/trees/pr-ctpop
B=/home/ubuntu/llvm/builds/pr-ctpop

cmake -S "$W/course2" -B "$B/course2" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$B/course2"
"$W/course2/scripts/course2-verify.sh" "$B/bin" "$B/course2/rsim"
```

## 预期输出

```text
10 checks
```

手工看一个文件：

```text
$ "$B/course2/rsim" --dump-elf /tmp/demo
entry 0x00010000
load vaddr=0x00010000 filesz=0x000004f4 memsz=0x000004f4 flags=0x5 align=0x1000
load vaddr=0x000104f4 filesz=0x00000020 memsz=0x00000020 flags=0x4 align=0x1000
load vaddr=0x00010518 filesz=0x0000003c memsz=0x0000003c flags=0x6 align=0x1000
```

拒绝路径：

```text
$ rsim --dump-elf /tmp/bad-magic ; echo $?
rsim: /tmp/bad-magic: bad magic (not an ELF file)
125
```

## 常见坑

1. **`e_phentsize` 可以大于 32。** 不要按 32 硬编码步长，要按
   `phoff + i * phentsize` 走；本章要求至少 32，忽略多出来的字节。
2. **`readelf` 的 Flg 列会拆成两个字段。** `R E` 在 `awk` 里是两个列，
   用正则整行匹配（或 Python）才不会把 `R` 当成对齐列。
3. **`p_filesz = 0` 的段是合法的。** 它是纯 `.bss`；加载器必须映射
   `p_memsz` 并清零，而不是跳过。第一课第 22 章踩过 QEMU 的相反实现：
   纯 `.bss` 段与 `.text` 共页时整页被清零。
4. **`ET_REL` 不是可执行文件。** `llvm-mc -filetype=obj` 的产物 `e_type=1`，
   必须被拒绝，否则入口点毫无意义。
5. **`e_entry` 落在段外要拒绝。** 只检查“有 PT_LOAD”不够；入口可以是 0
   或指向 `.rodata`，那一定跑不起来。
6. **错误信息要带文件名。** 验证脚本只检查 `^rsim: ` 前缀和退出码 125，
   但人工调试时 `rsim: <path>: <reason>` 比单独一句更有用。

## 练习

1. 支持 `PT_GNU_STACK` 并把它的 `PF_X` 记录到 `Image`，为第 08 章的
   “不能执行栈”检查做准备。
2. 增加 `--dump-segments` 的 JSON 输出，用 `python3 -m json.tool` 校验。
3. 构造一个 `p_filesz > p_memsz` 的 ELF，确认加载器在映射前就拒绝。
4. 把 `Memory` 换成 `mmap` + `MAP_FIXED_NOREPLACE` 的实现，比较两种
   页表在 4 GiB 地址空间下的内存占用（`/proc/self/status` 的 VmRSS）。
