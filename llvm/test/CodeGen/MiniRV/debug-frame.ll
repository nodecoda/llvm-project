; RUN: llc -mtriple=minirv64 -force-dwarf-frame-section -filetype=obj < %s -o %t.o
; RUN: llvm-readobj --sections %t.o | FileCheck %s --check-prefix=SECTIONS
; RUN: llvm-readobj --relocations %t.o | FileCheck %s --check-prefix=RELOC
; RUN: llvm-dwarfdump --debug-frame %t.o | FileCheck %s --check-prefix=FRAME

; ForceDwarfFrameSection switches the unwind table to .debug_frame.  Unlike
; .eh_frame, its PC begin is an ordinary address relocation (R_RISCV_64), not
; the R_RISCV_32_PCREL symbol difference that .eh_frame needs.

; SECTIONS: Name: .debug_frame
; RELOC:    Section {{.*}}.rela.debug_frame
; RELOC:    R_RISCV_64
; RELOC-NOT: R_RISCV_ADD

; FRAME:      CIE
; FRAME:      DW_CFA_def_cfa: X2 +0
; FRAME:      FDE
; FRAME:      DW_CFA_def_cfa_offset: +16
; FRAME:      DW_CFA_offset: X1 -8
; FRAME:      DW_CFA_restore: X1
; FRAME:      DW_CFA_def_cfa_offset: +0

declare void @callee()

define void @caller() #0 {
  call void @callee()
  ret void
}

attributes #0 = { nounwind }
