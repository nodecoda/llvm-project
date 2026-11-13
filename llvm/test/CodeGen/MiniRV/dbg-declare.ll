; RUN: llc -mtriple=minirv64 < %s | FileCheck %s --check-prefix=ASM
; RUN: llc -mtriple=minirv64 -filetype=obj < %s -o %t.o
; RUN: llvm-dwarfdump --debug-info %t.o | FileCheck %s --check-prefix=INFO

; A dbg.declare on an alloca becomes a DW_TAG_variable whose location is
; DW_OP_fbreg relative to the frame base.  The alloca is placed at SP+8 and
; the frame base is the current X2, so the variable lives at fbreg +8.

; ASM-LABEL: f:
; ASM:         addi    x2, x2, -16
; ASM-NEXT:    .cfi_def_cfa_offset 16
; ASM-NEXT:    addi    x3, x2, 8
; ASM:         sd      x10, 0(x3)

; INFO:       DW_TAG_subprogram
; INFO:       DW_AT_frame_base      (DW_OP_reg2 X2)
; INFO:       DW_TAG_formal_parameter
; INFO:       DW_AT_location        (DW_OP_fbreg +8)
; INFO:       DW_AT_name    ("p")

define i64 @f(i64 %a) !dbg !4 {
entry:
  %p = alloca i64, align 8
  call void @llvm.dbg.declare(metadata ptr %p, metadata !10, metadata !DIExpression()), !dbg !7
  store i64 %a, ptr %p, !dbg !7
  %v = load i64, ptr %p, !dbg !8
  ret i64 %v, !dbg !8
}

declare void @llvm.dbg.declare(metadata, metadata, metadata)

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3}
!0 = distinct !DICompileUnit(language: DW_LANG_C99, file: !1, producer: "clang", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "t.c", directory: "/tmp")
!2 = !{i32 7, !"Dwarf Version", i32 5}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = distinct !DISubprogram(name: "f", scope: !1, file: !1, line: 1, type: !5, scopeLine: 1, spFlags: DISPFlagDefinition, unit: !0)
!5 = !DISubroutineType(types: !6)
!6 = !{!9, !9}
!7 = !DILocation(line: 2, column: 3, scope: !4)
!8 = !DILocation(line: 3, column: 3, scope: !4)
!9 = !DIBasicType(name: "long", size: 64, encoding: DW_ATE_signed)
!10 = !DILocalVariable(name: "p", arg: 1, scope: !4, file: !1, line: 2, type: !9)
