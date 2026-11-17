# Zicsr + Zifencei encoding corpus for the RVMin MC layer (chapter 46).
#
# check-rvmin-encodings.sh assembles this file with both -triple=rvmin and
# -triple=riscv32 using -mattr=+zicsr,+zifencei,+f and requires the 32-bit
# encodings to match one for one.  The corpus covers:
#
#   * the six canonical CSR instructions, register and immediate forms;
#   * the seven standard pseudo-instructions (csrr/csrw/csrs/csrc/csrwi/
#     csrsi/csrci) plus the immediate spellings of csrw/csrs/csrc;
#   * named CSRs from the machine file (mstatus/mtvec/mepc/mcause) and the
#     floating-point CSRs (fflags/frm/fcsr);
#   * a numeric CSR number, which must produce the same bits as the name;
#   * fence.i, which moved from A to Zifencei in this chapter.
csrrw a0, mstatus, a1
csrrs a0, mstatus, a1
csrrc a0, mstatus, a1
csrrwi a0, mstatus, 5
csrrsi a0, mstatus, 5
csrrci a0, mstatus, 5
csrr a0, mstatus
csrw mtvec, a1
csrs mepc, a1
csrc mcause, a1
csrwi mstatus, 0
csrsi mstatus, 8
csrci mstatus, 8
csrrw a0, fflags, a1
csrrs a0, frm, a1
csrrc a0, fcsr, a1
csrr a0, fcsr
csrw fflags, a1
csrs frm, a1
csrc fcsr, a1
csrwi fflags, 31
csrr a0, 0x300
csrrw a0, 0x341, a1
fence.i
