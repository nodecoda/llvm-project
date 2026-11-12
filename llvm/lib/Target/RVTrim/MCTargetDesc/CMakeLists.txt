add_llvm_component_library(LLVMRVTrimDesc
  RVTrimAsmBackend.cpp
  RVTrimBaseInfo.cpp
  RVTrimELFObjectWriter.cpp
  RVTrimInstPrinter.cpp
  RVTrimMCAsmInfo.cpp
  RVTrimMCCodeEmitter.cpp
  RVTrimMCExpr.cpp
  RVTrimMCObjectFileInfo.cpp
  RVTrimMCTargetDesc.cpp
  RVTrimMachObjectWriter.cpp
  RVTrimMatInt.cpp
  RVTrimTargetStreamer.cpp
  RVTrimELFStreamer.cpp

  LINK_COMPONENTS
  BinaryFormat
  MC
  Option
  RVTrimInfo
  Support
  TargetParser

  ADD_TO_COMPONENT
  RVTrim
)
