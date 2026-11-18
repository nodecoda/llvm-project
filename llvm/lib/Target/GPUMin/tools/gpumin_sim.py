#!/usr/bin/env python3
"""A small GM32 simulator for the GPUMin tutorial.

The simulator is deliberately a *model*, not a cycle-accurate machine.  It
reads the ELF32 that llc writes, decodes the 32-bit words, and runs one warp
at a time with the state ownership frozen in chapter 11:

    PC + active mask     per warp
    r0-r31, p0-p7        per lane
    ur0-ur7              per warp
    shared memory        per block
    private memory       per lane
    global / constant    per grid

Chapter 12 uses it to run vecadd and the chapter 05/06/07/09 samples.  The
legacy "arguments in r4-r11" form stays for those samples; chapter 13 adds
`--launch`, which writes an argument block into global memory and exposes its
address through `sr.args` (backed by ur0).
"""

import argparse
import json
import struct
import sys

from gpumin_model import Model

MASK32 = 0xFFFFFFFF
WARP_SIZE = 8


def s32(value):
    value &= MASK32
    return value - (1 << 32) if value & 0x80000000 else value


def sext(value, bits):
    sign = 1 << (bits - 1)
    value &= (1 << bits) - 1
    return value - (1 << bits) if value & sign else value


class SimError(Exception):
    pass


class ELFImage:
    """The bits of ELF32 the simulator needs: sections and symbols."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        if self.data[:4] != b"\x7fELF":
            raise SimError("not an ELF file")
        if self.data[4] != 1 or self.data[5] != 1:
            raise SimError("expected ELF32 little-endian")

        (self.e_shoff,) = struct.unpack_from("<I", self.data, 0x20)
        (self.e_shentsize,) = struct.unpack_from("<H", self.data, 0x2E)
        (self.e_shnum,) = struct.unpack_from("<H", self.data, 0x30)
        (self.e_shstrndx,) = struct.unpack_from("<H", self.data, 0x32)

        self.sections = []
        for i in range(self.e_shnum):
            off = self.e_shoff + i * self.e_shentsize
            name, typ, flags, addr, offset, size, link, info, align, entsize = \
                struct.unpack_from("<IIIIIIIIII", self.data, off)
            self.sections.append({
                "name_off": name, "type": typ, "flags": flags, "addr": addr,
                "offset": offset, "size": size, "link": link, "info": info,
                "align": align, "entsize": entsize,
            })

        shstr = self.sections[self.e_shstrndx]
        self.shstr = self.data[shstr["offset"]:shstr["offset"] + shstr["size"]]
        for sec in self.sections:
            sec["name"] = self._string(sec["name_off"], self.shstr)

        self.text = self._section(".text")
        if self.text is None:
            raise SimError("no .text section")
        self.text_addr = self.text["addr"]
        self.text_bytes = bytearray(
            self.data[self.text["offset"]:
                      self.text["offset"] + self.text["size"]])
        if len(self.text_bytes) % 4:
            raise SimError(".text is not a multiple of four bytes")

        self.symbols = {}
        self.symbol_list = []
        self._parse_symbols()

        # An object file is not a program yet: `call` leaves an
        # R_GPUMIN_21_PCREL relocation behind because the callee address is not
        # known until link time.  KMVP has no linker, so the loader is a tiny
        # static linker: resolve relocations that target .text against the
        # symbol table.  Relocations against .eh_frame are unwind metadata and
        # do not affect execution.
        self._apply_relocations()

    def _string(self, off, table):
        end = table.find(b"\0", off)
        if end < 0:
            end = len(table)
        return table[off:end].decode("utf-8", "replace")

    def _section(self, name):
        for sec in self.sections:
            if sec["name"] == name and sec["type"] == 1:  # PROGBITS
                return sec
        return None

    def _parse_symbols(self):
        symtab = None
        strtab = None
        for sec in self.sections:
            if sec["name"] == ".symtab":
                symtab = sec
            elif sec["name"] == ".strtab":
                strtab = sec
        if not symtab or not strtab:
            return
        strings = self.data[strtab["offset"]:strtab["offset"] + strtab["size"]]
        entsize = symtab["entsize"] or 16
        count = symtab["size"] // entsize
        self.symbol_list = [None] * count
        for i in range(count):
            off = symtab["offset"] + i * entsize
            name, value, size, info, other, shndx = struct.unpack_from(
                "<IIIBBH", self.data, off)
            name = self._string(name, strings)
            sym = {"name": name, "value": value, "size": size, "info": info,
                   "shndx": shndx}
            self.symbol_list[i] = sym
            if name:
                self.symbols[name] = sym

    def _resolve_symbol(self, index):
        if index <= 0 or index >= len(self.symbol_list):
            raise SimError("relocation against invalid symbol %d" % index)
        sym = self.symbol_list[index]
        if sym is None:
            raise SimError("relocation against a null symbol")
        # Section-relative symbols carry an offset in st_value; absolute
        # symbols already carry the final address (SHN_ABS = 0xfff1).
        if sym["shndx"] == 0:
            raise SimError("relocation against undefined symbol %r" %
                           sym["name"])
        if sym["shndx"] == 0xFFF1:
            return sym["value"]
        if sym["shndx"] >= len(self.sections):
            raise SimError("symbol %r has a bad section index" % sym["name"])
        return self.sections[sym["shndx"]]["addr"] + sym["value"]

    def _apply_relocations(self):
        text_index = self.sections.index(self.text)
        for sec in self.sections:
            if sec["type"] != 4 or sec["info"] != text_index:  # SHT_RELA
                continue
            entsize = sec["entsize"] or 12
            count = sec["size"] // entsize
            for i in range(count):
                off = sec["offset"] + i * entsize
                r_offset, r_info, addend = struct.unpack_from(
                    "<IIi", self.data, off)
                sym_index = r_info >> 8
                rtype = r_info & 0xFF
                if rtype == 0:  # R_GPUMIN_NONE
                    continue
                if r_offset % 4 or r_offset + 4 > len(self.text_bytes):
                    raise SimError("bad relocation offset 0x%x" % r_offset)
                S = self._resolve_symbol(sym_index)
                P = self.text_addr + r_offset
                if rtype == 1:      # R_GPUMIN_32
                    value = S + addend
                elif rtype == 2:    # R_GPUMIN_32_PCREL
                    value = S + addend - P
                elif rtype in (3, 4):  # 16/21-bit PC-relative, in words
                    delta = S + addend - P
                    if delta & 3:
                        raise SimError(
                            "unaligned branch target at 0x%x" % r_offset)
                    value = delta // 4
                    if rtype == 3:
                        if not -(1 << 15) <= value < (1 << 15):
                            raise SimError(
                                "16-bit branch target out of range at 0x%x" %
                                r_offset)
                        word = struct.unpack_from("<I", self.text_bytes,
                                                  r_offset)[0]
                        word = (word & ~0xFFFF) | (value & 0xFFFF)
                        struct.pack_into("<I", self.text_bytes, r_offset, word)
                    else:
                        if not -(1 << 20) <= value < (1 << 20):
                            raise SimError(
                                "21-bit call target out of range at 0x%x" %
                                r_offset)
                        word = struct.unpack_from("<I", self.text_bytes,
                                                  r_offset)[0]
                        word = (word & ~0x1FFFFF) | (value & 0x1FFFFF)
                        struct.pack_into("<I", self.text_bytes, r_offset, word)
                    continue
                else:
                    raise SimError("unsupported relocation type %d at 0x%x" %
                                   (rtype, r_offset))
                struct.pack_into("<I", self.text_bytes, r_offset,
                                 value & MASK32)

    def word(self, pc):
        if pc < 0 or pc * 4 + 4 > len(self.text_bytes):
            raise SimError("PC %d outside .text" % pc)
        return struct.unpack_from("<I", self.text_bytes, pc * 4)[0]


class Lane:
    def __init__(self, laneid, warpid, blockid, threads, nwarps, private_size):
        self.laneid = laneid
        self.warpid = warpid
        self.blockid = blockid
        self.tid = warpid * WARP_SIZE + laneid
        self.ntid = threads
        self.nwarps = nwarps
        self.r = [0] * 32
        self.p = [0] * 8
        self.private = bytearray(private_size)
        self.r[30] = private_size          # chapter 07: r30 = sp
        self.r[31] = 0                      # lr; a call overwrites it

    def read_r(self, idx):
        return 0 if idx == 0 else self.r[idx]

    def write_r(self, idx, value):
        if idx != 0:
            self.r[idx] = value & MASK32

    def read_p(self, idx):
        return self.p[idx] & 1

    def write_p(self, idx, value):
        self.p[idx] = value & 1


class Warp:
    def __init__(self, lanes, pc, ur0=0):
        self.lanes = lanes
        # A frame is (pc, lane mask).  Chapter 12 kept one PC per warp; chapter
        # 14 allows several frames once lanes take different branches.
        self.frames = [(pc, (1 << len(lanes)) - 1)]
        self.pc = pc
        self.mask = self.frames[0][1]
        self.ur = [0] * 8
        self.ur[0] = ur0            # chapter 13: args pointer
        self.steps = 0              # instructions retired by this warp
        self.stall = 0              # chapter 19: cycles left before issuing

    def _normalize(self):
        # Frames that reached the same instruction can run together: every
        # lane owns its registers, predicates and private stack, so the union
        # mask has exactly the same effect as running them one after another.
        # This is the dynamic reconvergence point.
        merged = {}
        for pc, mask in self.frames:
            if mask:
                merged[pc] = merged.get(pc, 0) | mask
        self.frames = sorted(merged.items())

    def peek(self):
        """The frame that would run next, without moving the warp."""
        self._normalize()
        return self.frames[0] if self.frames else None

    def select(self):
        # Run the smallest PC first.  A path that reaches the join early waits
        # there because the other path is still at a smaller PC; when both are
        # at the join, _normalize() merges them.  No static post-dominator is
        # required for structured if/else.
        self._normalize()
        if not self.frames:
            return False
        self.pc, self.mask = self.frames[0]
        return True

    def set_current(self, frames):
        self.frames.remove((self.pc, self.mask))
        self.frames.extend((pc, mask) for pc, mask in frames if mask)

    def advance(self):
        self.set_current([(self.pc + 1, self.mask)])

    def jump(self, pc):
        self.set_current([(pc, self.mask)])

    def split(self, groups):
        self.set_current(list(groups.items()))

    def active_lanes(self):
        return [i for i in range(len(self.lanes)) if self.mask & (1 << i)]


class Memory:
    def __init__(self, global_size, private_size, shared_size):
        self.global_mem = bytearray(global_size)
        self.constant = bytearray(global_size)
        self.private_size = private_size
        self.shared_size = shared_size

    def load(self, space, lane, shared, addr, size, signed):
        if addr < 0:
            raise SimError("negative address 0x%x" % addr)
        if space == 0:  # global
            buf = self.global_mem
        elif space == 1:  # shared
            buf = shared
        elif space == 2:  # local / private
            buf = lane.private
        elif space == 3:  # constant
            buf = self.constant
        else:
            raise SimError("bad address space %d" % space)
        if addr + size > len(buf):
            raise SimError("load out of range: space=%d addr=0x%x size=%d" %
                           (space, addr, size))
        value = int.from_bytes(buf[addr:addr + size], "little")
        if signed and size < 4:
            value = sext(value, size * 8)
        return value & MASK32

    def store(self, space, lane, shared, addr, size, value):
        if addr < 0:
            raise SimError("negative address 0x%x" % addr)
        if space == 0:
            buf = self.global_mem
        elif space == 1:
            buf = shared
        elif space == 2:
            buf = lane.private
        elif space == 3:
            cur = self.load(3, lane, shared, addr, size, False)
            # constant is read-only; stores are ignored rather than faulting
            return cur
        else:
            raise SimError("bad address space %d" % space)
        if addr + size > len(buf):
            raise SimError("store out of range: space=%d addr=0x%x size=%d" %
                           (space, addr, size))
        buf[addr:addr + size] = (value & ((1 << (8 * size)) - 1)).to_bytes(
            size, "little")


class Simulator:
    def __init__(self, image, *, blocks=1, threads=32, trace=False,
                 max_steps=100000, global_size=1 << 20, private_size=4096,
                 shared_size=49152, strict_uniform=False, schedule="units",
                 unit_width=None, latency=None, regs_per_thread=None):
        if threads % WARP_SIZE or threads > 32:
            raise SimError("--threads must be a multiple of 8 and <= 32")
        self.image = image
        self.blocks = blocks
        self.threads = threads
        self.nwarps = threads // WARP_SIZE
        self.trace = trace
        self.max_steps = max_steps
        self.strict_uniform = strict_uniform
        # Chapter 17: how the block hands out issue slots.  "interleaved" runs
        # one instruction from each warp in turn; "greedy" lets a warp run
        # until it finishes or parks at a barrier.  Both are valid schedules,
        # which is exactly why a program that only works under one of them is
        # a race, not a program.
        if schedule not in ("units", "interleaved", "greedy"):
            raise SimError("--schedule must be units, interleaved or greedy")
        self.schedule = schedule
        self.model = Model()
        # Chapter 18: issue slots per cycle per unit.  "units" is the
        # quantitative model; the two chapter 17 modes stay available because
        # they are the extreme schedules a race has to survive.
        self.unit_width = self.model.issue_units
        for name, width in (unit_width or {}).items():
            self.unit_width[name] = width
        # Chapter 19: a load stalls its warp for a fixed number of cycles.
        # There is no scoreboard yet, so the whole warp waits even if the
        # consumer is several instructions away -- that is precisely why
        # occupancy matters.
        self.latency = self.model.latency
        if latency is not None:
            self.latency = {"global": latency, "shared": latency}
        # How many warps an SM can hold, given the registers each thread uses.
        self.warps_per_sm = self.model.warps_per_sm(regs_per_thread)
        self.memory = Memory(global_size, private_size, shared_size)
        self.shared = bytearray(shared_size)
        # Chapter 15: one place that counts what the program made the machine
        # do.  The numbers are the acceptance evidence: they must move when the
        # program changes and stay put when it does not.
        self.stats = {
            "issued": 0,
            "lane_instrs": 0,
            "loads": 0,
            "stores": 0,
            "branches": 0,
            "splits": 0,
            "frames_max": 0,
            "barriers": 0,
            "cycles": 0,
            "alu": 0,
            "lsu": 0,
            "bar": 0,
            "ld_ops": 0,
            "st_ops": 0,
            "stall_cycles": 0,
            "warps_per_sm": 0,
        }
        self.stats["warps_per_sm"] = self.warps_per_sm
        self.registers = {}
        self.dumps = []
        self.args_addr = None
        self.launch_dumps = []

    def find(self, name):
        if name not in self.image.symbols:
            raise SimError("symbol %s not found" % name)
        sym = self.image.symbols[name]
        if sym["value"] < self.image.text_addr:
            raise SimError("symbol %s is not in .text" % name)
        return (sym["value"] - self.image.text_addr) // 4

    def apply_launch(self, desc):
        if not isinstance(desc, dict):
            raise SimError("launch descriptor must be a JSON object")
        if desc.get("grid_dims", 1) != 1:
            raise SimError("only a 1D grid is supported")
        grid = desc.get("grid", 1)
        block = desc.get("block", self.threads)
        if not isinstance(grid, int) or isinstance(grid, bool) or grid < 1:
            raise SimError("launch grid must be a positive integer")
        if (not isinstance(block, int) or isinstance(block, bool) or
                block % WARP_SIZE or not 8 <= block <= 32):
            raise SimError("launch block must be a multiple of 8 in 8..32")
        self.blocks = grid
        self.threads = block
        self.nwarps = block // WARP_SIZE

        args = desc.get("args", [])
        if not isinstance(args, list):
            raise SimError("launch args must be a list of words")
        args_addr = desc.get("args_addr", 0x100)
        if (not isinstance(args_addr, int) or isinstance(args_addr, bool) or
                args_addr < 0 or args_addr % 4):
            raise SimError("args_addr must be a non-negative aligned integer")
        if args_addr + 4 * len(args) > len(self.memory.global_mem):
            raise SimError("launch args do not fit in global memory")
        for i, value in enumerate(args):
            if not isinstance(value, int) or isinstance(value, bool):
                raise SimError("launch args must be integers")
            self.memory.global_mem[args_addr + 4 * i:
                                   args_addr + 4 * i + 4] = \
                (value & MASK32).to_bytes(4, "little")
        self.args_addr = args_addr

        for spec in desc.get("fill", []):
            try:
                self.fill(int(spec["addr"]), int(spec["start"]),
                          int(spec["step"]), int(spec["count"]))
            except (KeyError, TypeError, ValueError):
                raise SimError("launch fill entries need addr/start/step/count")
        for spec in desc.get("dump", []):
            try:
                self.launch_dumps.append((int(spec["addr"]), int(spec["count"])))
            except (KeyError, TypeError, ValueError):
                raise SimError("launch dump entries need addr/count")

    def fill(self, addr, start, step, count):
        for i in range(count):
            self.memory.global_mem[addr + i * 4:addr + i * 4 + 4] = \
                ((start + i * step) & MASK32).to_bytes(4, "little")

    def dump(self, addr, count):
        return [int.from_bytes(self.memory.global_mem[addr + i * 4:
                                                     addr + i * 4 + 4],
                               "little")
                for i in range(count)]

    def run(self, kernel, args=()):
        entry = self.find(kernel)
        for blockid in range(self.blocks):
            self.shared = bytearray(self.memory.shared_size)
            warps = []
            for w in range(self.nwarps):
                lanes = [Lane(i, w, blockid, self.threads, self.nwarps,
                              self.memory.private_size)
                         for i in range(WARP_SIZE)]
                if len(args) > 8:
                    raise SimError("at most 8 arguments fit r4-r11")
                for i, value in enumerate(args):
                    for lane in lanes:
                        lane.write_r(4 + i, value)
                warps.append(Warp(lanes, entry, self.args_addr or 0))
            self._run_block(warps)

    def _run_block(self, warps):
        """Chapter 17: a block scheduler with a real barrier.

        A warp either runs a slice of instructions or parks at `bar.sync`.
        The barrier releases when every *live* warp has arrived; a warp that
        exited no longer blocks the rest.  That choice is the model's, not the
        hardware's: CUDA leaves the exited-lane case undefined, and this
        simulator makes it defined so a reduction can shrink its grid.
        """
        n = len(warps)
        done = [False] * n
        waiting = [False] * n
        while not all(done):
            if self.schedule == "units":
                # A cycle is one issue round.  A warp may take at most one
                # slot, and each unit has a fixed number of slots.
                self.stats["cycles"] += 1
                slots = dict(self.unit_width)
            for i, warp in enumerate(warps):
                if done[i] or waiting[i]:
                    continue
                if warp.stall:
                    # Chapter 19: the warp cannot issue until the load's
                    # result lands.  The counter is decremented here because
                    # this loop runs exactly once per warp per cycle.
                    warp.stall -= 1
                    self.stats["stall_cycles"] += 1
                    continue
                if self.schedule == "units":
                    unit = self._classify(warp)
                    if slots[unit] == 0:
                        # The warp is ready but the unit is busy.  Not a
                        # barrier: it will try again next cycle.
                        continue
                    slots[unit] -= 1
                steps = None if self.schedule == "greedy" else 1
                status = self._run_warp(warp, steps=steps)
                if status == "done":
                    done[i] = True
                elif status == "barrier":
                    waiting[i] = True
            if any(waiting) and all(done[i] or waiting[i]
                                    for i in range(n)):
                self.stats["barriers"] += 1
                waiting = [False] * n

    def _unit(self, word):
        """Chapter 18: which execution unit a word belongs to."""
        op = word >> 26
        if op in (0x08, 0x09):
            return "lsu"
        if op == 0x12 and (word & 0x7FF) == self.model.barrier_funct:
            return "bar"
        return "alu"

    def _classify(self, warp):
        frame = warp.peek()
        if frame is None:
            return "alu"
        return self._unit(self.image.word(frame[0]))

    def _special(self, warp, lane, name, mask):
        if name == "args":
            if self.args_addr is None:
                raise SimError("sr.args read without a launch descriptor")
            return warp.ur[0]
        return {
            "laneid": lane.laneid,
            "warpid": lane.warpid,
            "blockid": lane.blockid,
            "tid": lane.tid,
            "ntid": lane.ntid,
            "nwarps": lane.nwarps,
            "activemask": mask,
            "shared": self.model.shared_base,
        }[name]

    def report(self):
        """One line of chapter 15 execution statistics."""
        s = self.stats
        return ("issued=%d lane_instrs=%d loads=%d stores=%d branches=%d "
                "splits=%d frames_max=%d barriers=%d cycles=%d "
                "alu=%d lsu=%d bar=%d ld_ops=%d st_ops=%d stall_cycles=%d "
                "warps_per_sm=%d" %
                (s["issued"], s["lane_instrs"], s["loads"], s["stores"],
                 s["branches"], s["splits"], s["frames_max"], s["barriers"],
                 s["cycles"], s["alu"], s["lsu"], s["bar"], s["ld_ops"],
                 s["st_ops"], s["stall_cycles"], s["warps_per_sm"]))

    def _run_warp(self, warp, steps=None):
        """Run until the warp finishes ("done"), parks at a barrier, or has
        executed `steps` instructions (the interleaved scheduler passes 1)."""
        ran = 0
        while True:
            if steps is not None and ran >= steps:
                return None
            if warp.steps >= self.max_steps:
                raise SimError("step limit reached")
            if not warp.select():
                return "done"
            warp.steps += 1
            ran += 1
            pc = warp.pc
            self.stats["issued"] += 1
            self.stats["lane_instrs"] += bin(warp.mask).count("1")
            if len(warp.frames) > self.stats["frames_max"]:
                self.stats["frames_max"] = len(warp.frames)
            word = self.image.word(pc)
            self.stats[self._unit(word)] += 1
            if self.trace:
                print("warp b%d w%d pc=%d mask=%02x frames=%d word=%08x" %
                      (warp.lanes[0].blockid, warp.lanes[0].warpid, pc,
                       warp.mask, len(warp.frames), word), file=sys.stderr)
            op = word >> 26
            rd = (word >> 21) & 31
            rs1 = (word >> 16) & 31
            rs2 = (word >> 11) & 31

            if op == 0x01:  # MOV rd, rs1 (funct 0)
                for i in warp.active_lanes():
                    warp.lanes[i].write_r(rd, warp.lanes[i].read_r(rs1))
                warp.advance()
                continue
            if op == 0x05:  # MOVI
                imm = sext(word & 0xFFFF, 16)
                for i in warp.active_lanes():
                    warp.lanes[i].write_r(rd, imm)
                warp.advance()
                continue
            if op == 0x06:  # ADDI
                imm = sext(word & 0xFFFF, 16)
                for i in warp.active_lanes():
                    lane = warp.lanes[i]
                    lane.write_r(rd, lane.read_r(rs1) + imm)
                warp.advance()
                continue
            if op == 0x02:  # ALU
                funct = word & 0xF
                for i in warp.active_lanes():
                    lane = warp.lanes[i]
                    a, b = lane.read_r(rs1), lane.read_r(rs2)
                    if funct == 0:
                        v = a + b
                    elif funct == 1:
                        v = a - b
                    elif funct == 2:
                        v = a & b
                    elif funct == 3:
                        v = a | b
                    elif funct == 4:
                        v = a ^ b
                    elif funct == 5:
                        v = a << (b & 31)
                    elif funct == 6:
                        v = a >> (b & 31)
                    elif funct == 7:
                        v = s32(a) >> (b & 31)
                    else:
                        raise SimError("bad ALU funct %d" % funct)
                    lane.write_r(rd, v)
                warp.advance()
                continue
            if op == 0x03:  # SETP
                pd = (word >> 21) & 7
                funct = word & 0x7FF
                unsigned = bool(funct & 8)
                rel = funct & 7
                for i in warp.active_lanes():
                    lane = warp.lanes[i]
                    a, b = lane.read_r(rs1), lane.read_r(rs2)
                    if not unsigned:
                        a, b = s32(a), s32(b)
                    if rel == 0:
                        v = a == b
                    elif rel == 1:
                        v = a != b
                    elif rel == 2:
                        v = a < b
                    elif rel == 3:
                        v = a <= b
                    elif rel == 4:
                        v = a > b
                    elif rel == 5:
                        v = a >= b
                    else:
                        raise SimError("bad SETP relation %d" % rel)
                    lane.write_p(pd, 1 if v else 0)
                warp.advance()
                continue
            if op == 0x04:  # SELP
                pd = (word >> 8) & 7
                for i in warp.active_lanes():
                    lane = warp.lanes[i]
                    lane.write_r(rd, lane.read_r(rs1) if lane.read_p(pd)
                                 else lane.read_r(rs2))
                warp.advance()
                continue
            if op in (0x08, 0x09):  # LD / ST
                imm = sext((word >> 5) & 0x7FF, 11)
                space = (word >> 3) & 3
                size_code = (word >> 1) & 3
                sign = word & 1
                size = {0: 1, 1: 2, 2: 4}.get(size_code)
                if size is None:
                    raise SimError("bad memory size")
                base = (word >> 16) & 31
                lanes = warp.active_lanes()
                if op == 0x08:
                    self.stats["loads"] += len(lanes)
                    self.stats["ld_ops"] += 1
                else:
                    self.stats["stores"] += len(lanes)
                    self.stats["st_ops"] += 1
                if op == 0x08:
                    for i in lanes:
                        lane = warp.lanes[i]
                        value = self.memory.load(space, lane, self.shared,
                                                 lane.read_r(base) + imm,
                                                 size, bool(sign))
                        lane.write_r(rd, value)
                else:
                    val_reg = (word >> 21) & 31
                    for i in lanes:
                        lane = warp.lanes[i]
                        self.memory.store(space, lane, self.shared,
                                          lane.read_r(base) + imm, size,
                                          lane.read_r(val_reg))
                if op == 0x08 and self.schedule == "units":
                    # Global loads cost more than shared ones in the model.
                    warp.stall = self.latency[
                        "global" if space == 0 else "shared"]
                warp.advance()
                continue
            if op == 0x10:  # BRA
                has_pred = (word >> 25) & 1
                pred = (word >> 22) & 7
                off = sext(word & 0xFFFF, 16)
                taken = 0
                for i in warp.active_lanes():
                    lane = warp.lanes[i]
                    if not has_pred or lane.read_p(pred):
                        taken |= 1 << i
                not_taken = warp.mask & ~taken
                self.stats["branches"] += 1
                if taken and not_taken:
                    self.stats["splits"] += 1
                    if self.strict_uniform:
                        raise SimError(
                            "non-uniform branch at pc=%d; --strict-uniform is "
                            "the chapter 12 model" % pc)
                    warp.set_current([(pc + off, taken), (pc + 1, not_taken)])
                elif taken:
                    warp.jump(pc + off)
                else:
                    warp.jump(pc + 1)
                continue
            if op == 0x11:  # CALL
                off = sext(word & 0x1FFFFF, 21)
                for i in warp.active_lanes():
                    warp.lanes[i].write_r(31, pc + 1)
                warp.jump(pc + off)
                continue
            if op == 0x12:  # SYS
                funct = word & 0x7FF
                if funct == 0:  # EXIT: only this frame's lanes end
                    warp.set_current([])
                    if not warp.frames:
                        return "done"
                    continue
                if funct == self.model.barrier_funct:  # BAR.SYNC
                    # Every lane of the warp arrives together, so the park is
                    # whole-warp.  The scheduler decides when it leaves.
                    warp.advance()
                    return "barrier"
                if funct == 1:  # RET: lanes may return to different PCs
                    groups = {}
                    for i in warp.active_lanes():
                        lane = warp.lanes[i]
                        target = lane.read_r(31)
                        if target == 0:
                            raise SimError("ret with lr=0 at pc=%d" % pc)
                        groups[target] = groups.get(target, 0) | (1 << i)
                    warp.split(groups)
                    continue
                names = {v: k for k, v in self.model.special_funct.items()}
                names[self.model.args_funct] = "args"
                if funct in names:
                    name = names[funct]
                    for i in warp.active_lanes():
                        lane = warp.lanes[i]
                        lane.write_r(rd, self._special(warp, lane, name,
                                                       warp.mask))
                    warp.advance()
                    continue
                raise SimError("unknown SYS funct %d at pc=%d" % (funct, pc))
            raise SimError("unknown opcode 0x%x at pc=%d" % (op, pc))
        raise SimError("step limit reached")


def parse_int(text):
    return int(text, 0)


def main(argv=None):
    ap = argparse.ArgumentParser(description="GM32 simulator (teaching model)")
    ap.add_argument("elf", help="ELF32 object produced by llc")
    ap.add_argument("--kernel", help="kernel symbol")
    ap.add_argument("--blocks", type=parse_int, default=1)
    ap.add_argument("--threads", type=parse_int, default=32)
    ap.add_argument("--arg", action="append", type=parse_int, default=[],
                    help="legacy kernel argument for r4, r5, ... (repeatable)")
    ap.add_argument("--launch", metavar="FILE",
                    help="JSON launch descriptor (grid/block/args)")
    ap.add_argument("--fill", action="append", default=[],
                    metavar="ADDR=START,STEP,COUNT",
                    help="fill COUNT words at byte ADDR with START + i*STEP")
    ap.add_argument("--dump", action="append", default=[],
                    metavar="ADDR=COUNT", help="dump COUNT words after the run")
    ap.add_argument("--dump-text", action="store_true",
                    help="print the loaded .text words as hex, one per line")
    ap.add_argument("--trace", action="store_true")
    ap.add_argument("--schedule", choices=("units", "interleaved", "greedy"),
                    default="units",
                    help="block issue model (chapters 17-18)")
    ap.add_argument("--latency", type=parse_int, metavar="N",
                    help="cycles a load stalls its warp (chapter 19)")
    ap.add_argument("--regs-per-thread", type=parse_int, metavar="N",
                    help="registers per thread, for the occupancy report")
    ap.add_argument("--unit-width", action="append", default=[],
                    metavar="NAME=N",
                    help="override issue slots, e.g. alu=2 (repeatable)")
    ap.add_argument("--strict-uniform", action="store_true",
                    help="chapter 12 model: reject a divergent branch")
    ap.add_argument("--stats", action="store_true",
                    help="print execution statistics to stderr")
    ap.add_argument("--max-steps", type=parse_int, default=100000)
    args = ap.parse_args(argv)
    overrides = {}
    for spec in args.unit_width:
        try:
            name, value = spec.split("=", 1)
            if name not in ("alu", "lsu", "bar") or int(value) < 1:
                raise ValueError
        except ValueError:
            ap.error("--unit-width expects alu=N, lsu=N or bar=N with N >= 1")
        overrides[name] = int(value)

    if not args.dump_text and not args.kernel:
        ap.error("--kernel is required unless --dump-text is used")

    try:
        image = ELFImage(args.elf)
        if args.dump_text:
            for i in range(len(image.text_bytes) // 4):
                print("%08x" % image.word(i))
        if args.launch and args.arg:
            ap.error("--launch and --arg describe different kernel ABIs")
        sim = Simulator(image, blocks=args.blocks,
                        threads=args.threads, trace=args.trace,
                        max_steps=args.max_steps,
                        strict_uniform=args.strict_uniform,
                        schedule=args.schedule,
                        unit_width=overrides, latency=args.latency,
                        regs_per_thread=args.regs_per_thread)
        if args.launch:
            try:
                with open(args.launch, encoding="utf-8") as f:
                    sim.apply_launch(json.load(f))
            except json.JSONDecodeError as e:
                raise SimError("bad launch JSON: %s" % e)
        for spec in args.fill:
            addr_text, rest = spec.split("=", 1)
            start, step, count = (parse_int(x) for x in rest.split(","))
            sim.fill(parse_int(addr_text), start, step, count)
        if args.kernel:
            sim.run(args.kernel, args.arg)
        for addr, count in sim.launch_dumps:
            words = sim.dump(addr, count)
            print(" ".join("%d" % w if w < 2**31 else "%d" % s32(w)
                           for w in words))
        for spec in args.dump:
            addr_text, count = spec.split("=", 1)
            words = sim.dump(parse_int(addr_text), parse_int(count))
            print(" ".join("%d" % w if w < 2**31 else "%d" % s32(w)
                           for w in words))
        if args.stats and args.kernel:
            print(sim.report(), file=sys.stderr)
    except (SimError, OSError, ValueError) as e:
        print("simulator error: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
