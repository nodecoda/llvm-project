include_directories(
  ${PROJECT_SOURCE_DIR}/lib/Target/X86
  ${PROJECT_BINARY_DIR}/lib/Target/X86
  )

# Frozen atom corpus: the JSON is the reviewed source of truth, and the
# generated header carries its count and canonical hash so the C++ atom table
# cannot drift from it silently.
set(X86_CTPOP_ATOMS_JSON ${CMAKE_CURRENT_SOURCE_DIR}/x86-ctpop-atoms.json)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${X86_CTPOP_ATOMS_JSON})
file(READ ${X86_CTPOP_ATOMS_JSON} X86_CTPOP_ATOMS_CONTENT)
string(JSON X86_CTPOP_ATOM_COUNT GET "${X86_CTPOP_ATOMS_CONTENT}" count)
string(JSON X86_CTPOP_ATOMS_FNV1A64 GET "${X86_CTPOP_ATOMS_CONTENT}"
       canonical_fnv1a64)
configure_file(x86-ctpop-atoms.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/x86-ctpop-atoms.h @ONLY)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

set(LLVM_LINK_COMPONENTS
  Analysis
  AsmParser
  CodeGen
  CodeGenTypes
  Core
  GlobalISel
  InstCombine
  MC
  MIRParser
  Passes
  SelectionDAG
  Support
  Target
  TargetParser
  X86CodeGen
  X86Desc
  X86Info
  )

add_llvm_unittest(X86Tests
  MachineSizeOptsTest.cpp
  TernlogTest.cpp
  X86LegalizerInfoDifferentialTest.cpp
  X86LegalizerInfoReference.cpp
  X86SelectionDAGTest.cpp
  )
