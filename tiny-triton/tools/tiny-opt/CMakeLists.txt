set(LIBS
  MLIRArithDialect
  MLIRFuncDialect
  MLIRIR
  MLIROptLib
  MLIRRegisterAllPasses
  MLIRSCFDialect
  TinyTTGIR
  TinyTTIR
  )

add_llvm_executable(tiny-opt tiny-opt.cpp)
llvm_update_compile_flags(tiny-opt)
target_link_libraries(tiny-opt PRIVATE ${LIBS})
mlir_check_all_link_libraries(tiny-opt)
