set(LIBS
  MLIRArithDialect
  MLIRFuncDialect
  MLIRIR
  MLIRLLVMDialect
  MLIRParser
  MLIRPass
  MLIRSCFDialect
  MLIRSupport
  TinyTTGIR
  TinyTTGToLLVM
  TinyTTIR
  TinyTTToTinyTTG
  )

add_llvm_executable(tiny-triton main.cpp EmitTTIR.cpp PyAst.cpp)
llvm_update_compile_flags(tiny-triton)
target_link_libraries(tiny-triton PRIVATE ${LIBS})
mlir_check_all_link_libraries(tiny-triton)
