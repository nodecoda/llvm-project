# RUN: not llvm-mc -triple=rvtrim64 -mattr=+f -filetype=obj %s -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=F
# RUN: not llvm-mc -triple=rvtrim64 -mattr=+c -filetype=obj %s -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=C
# RUN: not llvm-mc -triple=rvtrim64 -mattr=+v -filetype=obj %s -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=V
# RUN: not llc -mtriple=rvtrim64 -mcpu=sifive-u74 %s -o /dev/null 2>&1 \
# RUN:   | FileCheck %s --check-prefix=CPU

# RVTrim implements exactly RV64IMA + Zicsr + Zifencei.  Everything else is
# rejected when the subtarget is created, not silently ignored.

# F: LLVM ERROR: RVTrim implements only RV64IMA (i, m, a, zicsr, zifencei); extension 'f' is outside the trimmed profile
# C: LLVM ERROR: RVTrim implements only RV64IMA (i, m, a, zicsr, zifencei); extension 'c' is outside the trimmed profile
# V: LLVM ERROR: RVTrim implements only RV64IMA (i, m, a, zicsr, zifencei); extension 'f' is outside the trimmed profile
# CPU: LLVM ERROR: RVTrim implements only RV64IMA (i, m, a, zicsr, zifencei); extension 'f' is outside the trimmed profile
