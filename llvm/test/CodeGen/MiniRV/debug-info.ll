; RUN: llc -mtriple=minirv64 -filetype=obj < %s -o %t.o
; RUN: llvm-readobj --sections %t.o | FileCheck %s --check-prefix=SECTIONS
; RUN: llvm-dwarfdump --debug-info %t.o | FileCheck %s --check-prefix=INFO
; RUN: llvm-dwarfdump --debug-line %t.o | FileCheck %s --check-prefix=LINE
; RUN: llvm-dwarfdump --eh-frame %t.o | FileCheck %s --check-prefix=EH

; DWARF is emitted as version 5 with the section-indexed forms the generic
; code produces.  The unwind table starts from the entry stack pointer.

; SECTIONS: Name: .debug_info
; SECTIONS: Name: .debug_str_offsets
; SECTIONS: Name: .debug_addr
; SECTIONS: Name: .debug_line

; INFO: DW_TAG_compile_unit
; INFO: DW_AT_name {{.*}}"t.c"
; INFO: DW_TAG_subprogram
; INFO: DW_AT_low_pc
; INFO: DW_AT_high_pc
; INFO: DW_AT_name {{.*}}"caller"

; LINE: 0x0000000000000018      2
; LINE: 0x000000000000001c      3

; EH: DW_CFA_def_cfa: X2 +0
; EH: DW_CFA_def_cfa_offset: +16
; EH: DW_CFA_offset: X1 -8
; EH: DW_CFA_restore: X1

; A second function with a real frame exercises the unwind moves; the first
; one is a leaf and needs none.
declare void @sink()

define void @withframe() #0 {
  call void @sink()
  ret void
}

define i32 @caller(i32 %a) !dbg !4 {
entry:
  %r = add i32 %a, 1, !dbg !7
  ret i32 %r, !dbg !8
}

attributes #0 = { uwtable }

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3}
!0 = distinct !DICompileUnit(language: DW_LANG_C99, file: !1, producer: "clang", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "t.c", directory: "/tmp")
!2 = !{i32 7, !"Dwarf Version", i32 5}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = distinct !DISubprogram(name: "caller", scope: !1, file: !1, line: 1, type: !5, scopeLine: 1, spFlags: DISPFlagDefinition, unit: !0)
!5 = !DISubroutineType(types: !6)
!6 = !{!9, !9}
!7 = !DILocation(line: 2, column: 10, scope: !4)
!8 = !DILocation(line: 3, column: 3, scope: !4)
!9 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
