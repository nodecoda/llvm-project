#!/usr/bin/env python3
"""An independent GM32 encoder, written from the ISA description only.

The point of this file is to *not* share code with LLVM.  A backend bug that
happens to match itself is invisible to a round-trip test inside the backend;
it is not invisible to a second implementation that was written from the same
specification.  `scripts/check-ch02.sh` compares this encoder against
`llvm-mc` instruction by instruction.

Everything below is chapter 06's bit layout:

    R      op[31:26] rd[25:21] rs1[20:16] rs2[15:11] funct[10:0]
    MOVI   op[31:26] rd[25:21] 0[20:16]            imm16[15:0]
    LD/ST  op[31:26] rd/rs2[25:21] base[20:16]     imm11[15:5]
           space[4:3] size[2:1] sign[0]
    SETP   op[31:26] pd[25:21] rs1[20:16] rs2[15:11] funct[10:0]

String literals are still chapter 01 strings where the format did not change;
the memory mnemonics became `ld.<space>.<size>` in chapter 06.
"""

import re
import sys

OPC = {
    "mov": 0x01,   # register form; MOVI has its own opcode
    "alu": 0x02,
    "setp": 0x03,
    "selp": 0x04,
    "movi": 0x05,
    "ld": 0x08,
    "st": 0x09,
    "bra": 0x10,
    "exit": 0x12,
}

ALU = {"add": 0, "sub": 1, "and": 2, "or": 3,
       "xor": 4, "shl": 5, "shr": 6, "sar": 7}

# funct = { unsigned, relation[2:0] }: bit 3 is the unsigned flag.
SETP = {"eq": 0, "ne": 1, "lt": 2, "le": 3, "gt": 4, "ge": 5,
        "ueq": 8, "une": 9, "ult": 10, "ule": 11, "ugt": 12, "uge": 13}

SPACE = {"global": 0, "shared": 1, "local": 2, "constant": 3}

# size field: 0 = 8 bits, 1 = 16 bits, 2 = 32 bits
SIZE = {"b8": 0, "b16": 1, "b32": 2}

# load suffixes: the sign bit is part of the mnemonic, not an extra operand
LOAD_KIND = {
    "u8": (0, 0), "s8": (0, 1),
    "u16": (1, 0), "s16": (1, 1),
    "b32": (2, 0),
}

STORE_KIND = {"b8": 0, "b16": 1, "b32": 2}

REG_RE = re.compile(r"^r(?:[12]?\d|3[01])$")
UREG_RE = re.compile(r"^ur[0-7]$")
PREG_RE = re.compile(r"^p[0-7]$")
MEM_RE = re.compile(r"^(-?(?:0x[0-9a-fA-F]+|\d+))\((r(?:[12]?\d|3[01]))\)$")


class AsmError(Exception):
    pass


def reg_number(name):
    """Return the 5-bit encoding of a per-lane register name."""
    if not REG_RE.match(name):
        raise AsmError("not a per-lane register: %s" % name)
    return int(name[1:])


def word_r(op, rd, rs1, rs2, funct):
    return (op << 26) | (rd << 21) | (rs1 << 16) | (rs2 << 11) | (funct & 0x7FF)


def word_i(op, rd, rs1, imm):
    if not -32768 <= imm <= 32767:
        raise AsmError("immediate out of range: %d" % imm)
    return (op << 26) | (rd << 21) | (rs1 << 16) | (imm & 0xFFFF)


def word_m(op, reg, base, imm, space, size, sign):
    """The M format: 11-bit displacement plus three qualifier bits.

    The displacement is a *byte* offset, and it is signed.  Anything outside
    +-1024 cannot be expressed; refusing here is what keeps a caller from
    silently loading from the wrong address."""
    if not -1024 <= imm <= 1023:
        raise AsmError("memory displacement out of range: %d" % imm)
    return ((op << 26) | (reg << 21) | (base << 16) |
            ((imm & 0x7FF) << 5) | (space << 3) | (size << 1) | sign)


def parse_imm(text):
    return int(text, 0)


def encode(text):
    """Encode one line of GM32 assembly into one 32-bit word."""
    line = text.strip()
    if not line or line.startswith("#"):
        return None

    parts = line.split(None, 1)
    mnemonic = parts[0].lower()
    operands = [o.strip() for o in parts[1].split(",")] if len(parts) > 1 else []

    if mnemonic == "exit":
        if operands:
            raise AsmError("exit takes no operands")
        return OPC["exit"] << 26

    if mnemonic == "mov":
        if len(operands) != 2:
            raise AsmError("mov needs rd, rs1")
        return word_r(OPC["mov"], reg_number(operands[0]),
                      reg_number(operands[1]), 0, 0)

    if mnemonic == "movi":
        if len(operands) != 2:
            raise AsmError("movi needs rd, imm")
        return word_i(OPC["movi"], reg_number(operands[0]), 0,
                      parse_imm(operands[1]))

    if mnemonic.startswith("ld."):
        parts = mnemonic.split(".")
        if len(parts) != 3 or parts[1] not in SPACE or parts[2] not in LOAD_KIND:
            raise AsmError("bad load mnemonic: %s" % mnemonic)
        if len(operands) != 2:
            raise AsmError("ld needs rd, imm(base)")
        m = MEM_RE.match(operands[1])
        if not m:
            raise AsmError("bad load operand: %s" % operands[1])
        size, sign = LOAD_KIND[parts[2]]
        return word_m(OPC["ld"], reg_number(operands[0]), reg_number(m.group(2)),
                      parse_imm(m.group(1)), SPACE[parts[1]], size, sign)

    if mnemonic.startswith("st."):
        parts = mnemonic.split(".")
        if len(parts) != 3 or parts[1] not in SPACE or parts[2] not in STORE_KIND:
            raise AsmError("bad store mnemonic: %s" % mnemonic)
        if len(operands) != 2:
            raise AsmError("st needs rs2, imm(base)")
        m = MEM_RE.match(operands[1])
        if not m:
            raise AsmError("bad store operand: %s" % operands[1])
        # The stored value rides in the rd slot; bit 0 is always zero.
        return word_m(OPC["st"], reg_number(operands[0]), reg_number(m.group(2)),
                      parse_imm(m.group(1)), SPACE[parts[1]],
                      STORE_KIND[parts[2]], 0)

    if mnemonic in ALU:
        if len(operands) != 3:
            raise AsmError("%s needs rd, rs1, rs2" % mnemonic)
        return word_r(OPC["alu"], reg_number(operands[0]),
                      reg_number(operands[1]), reg_number(operands[2]),
                      ALU[mnemonic])

    if mnemonic == "bra":
        # Only numeric word offsets: resolving labels is the assembler's job.
        if len(operands) == 1:
            pd, off_text = None, operands[0]
        elif len(operands) == 2:
            pd, off_text = operands[0], operands[1]
            if not PREG_RE.match(pd):
                raise AsmError("not a predicate register: %s" % pd)
        else:
            raise AsmError("bra needs [pd,] offset")
        off = parse_imm(off_text)
        if not -32768 <= off <= 32767:
            raise AsmError("branch offset out of range: %d" % off)
        word = (OPC["bra"] << 26) | (off & 0xFFFF)
        if pd:
            word |= (1 << 25) | (int(pd[1:]) << 22)
        return word

    if mnemonic == "selp":
        if len(operands) != 4:
            raise AsmError("selp needs rd, pd, rs1, rs2")
        pd = operands[1]
        if not PREG_RE.match(pd):
            raise AsmError("not a predicate register: %s" % pd)
        # opcode 0x04, and the predicate rides in bits [10:8].
        return (OPC["selp"] << 26) | (reg_number(operands[0]) << 21) | \
               (reg_number(operands[2]) << 16) | \
               (reg_number(operands[3]) << 11) | (int(pd[1:]) << 8)

    if mnemonic.startswith("setp."):
        cond = mnemonic.split(".", 1)[1]
        if cond not in SETP:
            raise AsmError("unknown setp condition: %s" % cond)
        if len(operands) != 3:
            raise AsmError("setp needs pd, rs1, rs2")
        pd = operands[0]
        if not PREG_RE.match(pd):
            raise AsmError("not a predicate register: %s" % pd)
        return word_r(OPC["setp"], int(pd[1:]), reg_number(operands[1]),
                      reg_number(operands[2]), SETP[cond])

    raise AsmError("unknown mnemonic: %s" % mnemonic)


def little_endian(word):
    return [(word >> (8 * i)) & 0xFF for i in range(4)]


def hex_bytes(word):
    return ",".join("0x%02x" % b for b in little_endian(word))


SELFTEST = [
    ("mov r1, r2", "0x00,0x00,0x22,0x04"),
    ("movi r3, 42", "0x2a,0x00,0x60,0x14"),
    ("add r4, r5, r6", "0x00,0x30,0x85,0x08"),
    ("sar r4, r5, r6", "0x07,0x30,0x85,0x08"),
    ("setp.lt p1, r2, r3", "0x02,0x18,0x22,0x0c"),
    ("setp.ugt p0, r4, r5", "0x0c,0x28,0x04,0x0c"),
    ("selp r4, p0, r2, r1", "0x00,0x08,0x82,0x10"),
    ("bra 2", "0x02,0x00,0x00,0x40"),
    ("bra p1, 0", "0x00,0x00,0x40,0x42"),
    ("ld.global.b32 r7, 8(r8)", "0x04,0x01,0xe8,0x20"),
    ("ld.global.s8 r1, -4(r2)", "0x81,0xff,0x22,0x20"),
    ("ld.shared.u16 r31, 1023(r30)", "0xea,0x7f,0xfe,0x23"),
    ("st.global.b32 r9, 4(r10)", "0x84,0x00,0x2a,0x25"),
    ("st.local.b8 r0, 0(r1)", "0x10,0x00,0x01,0x24"),
    ("st.shared.b16 r31, -1024(r30)", "0x0a,0x80,0xfe,0x27"),
    ("exit", "0x00,0x00,0x00,0x48"),
]


def selftest():
    for text, want in SELFTEST:
        got = hex_bytes(encode(text))
        if got != want:
            print("SELFTEST FAIL: %s -> %s, want %s" % (text, got, want))
            return 1
    print("PASS: Python encoder self-test (%d instructions)" % len(SELFTEST))
    return 0


def main(argv):
    if "--selftest" in argv:
        return selftest()

    path = None
    for arg in argv[1:]:
        if not arg.startswith("-"):
            path = arg
    lines = (open(path).read().splitlines() if path else sys.stdin.read().splitlines())

    try:
        for line in lines:
            word = encode(line)
            if word is None:
                continue
            print(hex_bytes(word))
    except AsmError as err:
        print("error: %s: %s" % (line.strip(), err), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
