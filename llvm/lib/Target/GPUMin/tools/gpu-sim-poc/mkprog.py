import sys, time

OPS = ["EXIT","MOVI","MOV","ADD","SUB","MUL","SHR","SETP_LT","BRA","LOAD_G","STORE_G","SYNC","SR","JMP","LOAD_S","STORE_S"]
OP = {n:i for i,n in enumerate(OPS)}

def I(op, rd=0, rs1=0, rs2=0, imm=0, pe=0, pr=0):
    return (OP[op], pe, pr, rd, rs1, rs2, imm)

def vecadd():
    p = [
        I("MOVI", rd=7, imm=4),            # 0  r7 = 4
        I("SR", rd=2, imm=0),              # 1  r2 = tid
        I("SR", rd=3, imm=1),              # 2  r3 = blockid
        I("MOVI", rd=5, imm=32),           # 3
        I("MUL", rd=3, rs1=3, rs2=5),      # 4  r3 = bid*32
        I("ADD", rd=1, rs1=2, rs2=3),      # 5  r1 = gid
        I("MUL", rd=8, rs1=1, rs2=7),      # 6  r8 = gid*4
        I("SR", rd=6, imm=4),              # 7  r6 = n
        I("SETP_LT", pr=0, rs1=1, rs2=6),  # 8  p0 = gid < n
        I("SR", rd=9, imm=5),              # 9  r9 = a base
        I("ADD", rd=11, rs1=9, rs2=8),     # 10 addr a
        I("SR", rd=12, imm=6),             # 11
        I("ADD", rd=13, rs1=12, rs2=8),    # 12 addr b
        I("SR", rd=14, imm=7),             # 13
        I("ADD", rd=15, rs1=14, rs2=8),    # 14 addr out
        I("LOAD_G", rd=16, rs1=11, pe=1, pr=0),
        I("LOAD_G", rd=17, rs1=13, pe=1, pr=0),
        I("ADD", rd=18, rs1=16, rs2=17, pe=1, pr=0),
        I("STORE_G", rs1=15, rs2=18, pe=1, pr=0),
        I("EXIT"),
    ]
    return p

def reduce():
    p = [
        I("MOVI", rd=7, imm=4),
        I("SR", rd=2, imm=0),              # tid
        I("SR", rd=3, imm=1),              # bid
        I("MOVI", rd=5, imm=32),
        I("MUL", rd=3, rs1=3, rs2=5),
        I("ADD", rd=1, rs1=2, rs2=3),      # gid
        I("MUL", rd=8, rs1=1, rs2=7),
        I("SR", rd=9, imm=5),
        I("ADD", rd=11, rs1=9, rs2=8),
        I("LOAD_G", rd=16, rs1=11),
        I("MUL", rd=12, rs1=2, rs2=7),     # shared byte off = tid*4
        I("STORE_S", rs1=12, rs2=16),
        I("SYNC"),
    ]
    for s in (16, 8, 4, 2, 1):
        p += [
            I("MOVI", rd=17, imm=s*4),
            I("SETP_LT", pr=0, rs1=12, rs2=17),
            I("ADD", rd=19, rs1=12, rs2=17, pe=1, pr=0),
            I("LOAD_S", rd=20, rs1=19, pe=1, pr=0),
            I("LOAD_S", rd=21, rs1=12, pe=1, pr=0),
            I("ADD", rd=22, rs1=21, rs2=20, pe=1, pr=0),
            I("STORE_S", rs1=12, rs2=22, pe=1, pr=0),
            I("SYNC"),
        ]
    p += [
        I("MOVI", rd=17, imm=4),
        I("SETP_LT", pr=0, rs1=12, rs2=17),  # tid == 0
        I("SR", rd=14, imm=7),
        I("SR", rd=3, imm=1),
        I("MUL", rd=3, rs1=3, rs2=7),
        I("ADD", rd=15, rs1=14, rs2=3),
        I("LOAD_S", rd=23, rs1=12, pe=1, pr=0),
        I("STORE_G", rs1=15, rs2=23, pe=1, pr=0),
        I("EXIT"),
    ]
    return p

VECADD = vecadd()
REDUCE = reduce()

def emit():
    out = ['typedef struct { unsigned char op, pe, pr, rd, rs1, rs2; short imm; } Inst;']
    for name, p in (("VECADD", VECADD), ("REDUCE", REDUCE)):
        out.append("static const Inst PROG_%s[%d] = {" % (name, len(p)))
        out.append("  " + ",\n  ".join("{ %d,%d,%d,%d,%d,%d,%d }" % t for t in p))
        out.append("};")
        out.append("#define PROG_%s_N %d" % (name, len(p)))
    return "\n".join(out) + "\n"

# ---------------- CPU reference interpreter ----------------
def run_prog(prog, tid, bid, sr, gmem, shared):
    r = [0]*32
    p = [0]*8
    pc = 0
    n = len(prog)
    while pc < n:
        op, pe, pr, rd, rs1, rs2, imm = prog[pc]
        if pe and not p[pr]:
            pc += 1
            continue
        if op == 0:
            break
        elif op == 1:  r[rd] = imm
        elif op == 2:  r[rd] = r[rs1]
        elif op == 3:  r[rd] = (r[rs1] + r[rs2])
        elif op == 4:  r[rd] = (r[rs1] - r[rs2])
        elif op == 5:  r[rd] = (r[rs1] * r[rs2])
        elif op == 6:  r[rd] = (r[rs1] >> imm) & 0xffffffff
        elif op == 7:  p[pr] = 1 if r[rs1] < r[rs2] else 0
        elif op == 8:  pc += imm; continue
        elif op == 9:  r[rd] = gmem[(r[rs1]) >> 2]
        elif op == 10: gmem[(r[rs1]) >> 2] = r[rs2]
        elif op == 11: pass
        elif op == 12: r[rd] = sr[imm]
        elif op == 13: pc += imm; continue
        elif op == 14: r[rd] = shared[(r[rs1]) >> 2]
        elif op == 15: shared[(r[rs1]) >> 2] = r[rs2]
        else: raise SystemExit("bad op %d" % op)
        pc += 1
    return r

def interp_vecadd(n):
    g = [0]*(3*n)
    for i in range(n):
        g[i] = i % 17
        g[n+i] = (i*3) % 19
    nblk = n // 32
    sr = [0,0,0,0,n,0,n*4,2*n*4]
    for b in range(nblk):
        sr[1] = b
        for t in range(32):
            sr[0] = t
            run_prog(VECADD, t, b, sr, g, [0]*32)
    return g

if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "emit"
    if mode == "emit":
        sys.stdout.write(emit())
    elif mode == "bench":
        n = int(sys.argv[2])
        t0 = time.time()
        g = interp_vecadd(n)
        dt = time.time() - t0
        lanes = (n//32)*32
        instrs = lanes * len(VECADD)
        bad = sum(1 for i in range(n) if g[2*n+i] != ((i % 17) + (i*3) % 19))
        print("python_interp n=%d time=%.3fs lanes=%d instrs=%d instr_per_s=%.0f mismatches=%d"
              % (n, dt, lanes, instrs, instrs/dt, bad))
