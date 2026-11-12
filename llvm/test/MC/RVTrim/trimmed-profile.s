# RUN: llvm-mc -triple=rvtrim64 -mattr=+zve64x -filetype=obj %s -o /dev/null
# RUN: not llvm-mc -triple=rvtrim64 -mattr=+f -filetype=obj %s -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=F
# RUN: not llvm-mc -triple=rvtrim64 -mattr=+c -filetype=obj %s -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=C
# RUN: not llvm-mc -triple=rvtrim64 -mattr=+v -filetype=obj %s -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=V
# RUN: not llvm-mc -triple=rvtrim64 -mattr=+zve32x -filetype=obj %s -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=ZVE32X
# RUN: not llc -mtriple=rvtrim64 -mcpu=sifive-u74 %s -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=CPU

# RVTrim implements exactly RV64IMA + Zicsr + Zifencei by default.  The
# optional RV64IMA_Zve64x profile adds the integer vector extension (zve64x and
# its implications).  Everything else is rejected when the subtarget is
# created, not silently ignored.  A bare +zve32x must not be accepted: it
# would enable half of the vector extension without 64-bit element support.

# F: LLVM ERROR: RVTrim implements only RV64IMA (i, m, a, zicsr, zifencei); extension 'f' is outside the trimmed profile
# C: LLVM ERROR: RVTrim implements only RV64IMA (i, m, a, zicsr, zifencei); extension 'c' is outside the trimmed profile
# V: LLVM ERROR: RVTrim implements only RV64IMA_Zve64x (i, m, a, zicsr, zifencei, zve64x); extension 'f' is outside the trimmed profile
# ZVE32X: LLVM ERROR: RVTrim implements only RV64IMA (i, m, a, zicsr, zifencei); extension 'zve32x' is outside the trimmed profile
# CPU: LLVM ERROR: RVTrim implements only RV64IMA (i, m, a, zicsr, zifencei); extension 'f' is outside the trimmed profile
