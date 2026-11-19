//===-- main.cpp - tiny-triton driver -------------------------------------===//
//
//   tiny-triton kernel.py --emit ttir --arg-type a_ptr=ptr ...
//   tiny-triton kernel.py --dump-ast
//   tiny-triton --ast-json kernel.ast.json --emit ttir ...
//
// Chapter 2 only implements `--emit ttir`; `--emit ttgir|llir` arrive in
// chapters 4 and 5 and are rejected here on purpose.
//
//===----------------------------------------------------------------------===//

#include "EmitTTIR.h"
#include "PyAst.h"

#include "tiny/Dialect/TinyTT/TinyTT.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/IR/MLIRContext.h"
#include <memory>

using namespace llvm;
using namespace mlir;
using namespace tinytriton;

static cl::opt<std::string> Input(cl::Positional, cl::desc("<kernel.py>"),
                                  cl::init(""));
static cl::opt<std::string> AstJson("ast-json",
                                    cl::desc("Read AST JSON instead of Python"),
                                    cl::value_desc("file"), cl::init(""));
static cl::opt<std::string> Python("python",
                                   cl::desc("Python interpreter to run"),
                                   cl::value_desc("path"), cl::init("python3"));
static cl::opt<std::string> Emit("emit", cl::desc("Output stage: ttir"),
                                 cl::value_desc("stage"), cl::init(""));
static cl::opt<std::string> Kernel("kernel",
                                   cl::desc("Kernel to lower (default: first)"),
                                   cl::value_desc("name"), cl::init(""));
static cl::list<std::string> ArgTypeSpecs(
    "arg-type", cl::desc("Type an argument: NAME=ptr|ptr<f32>|i32|f32|i1"),
    cl::value_desc("NAME=TYPE"));
static cl::opt<std::string> Output("o", cl::desc("Write output to <file>"),
                                   cl::value_desc("file"), cl::init("-"));

static int reportError(Error E) {
  errs() << "tiny-triton: " << toString(std::move(E)) << "\n";
  return 1;
}

static Expected<mlir::Type> parseTypeSpec(MLIRContext &Ctx, StringRef Spec) {
  if (Spec == "i1")
    return IntegerType::get(&Ctx, 1);
  if (Spec == "i32")
    return IntegerType::get(&Ctx, 32);
  if (Spec == "f32")
    return Float32Type::get(&Ctx);
  if (Spec == "ptr")
    return tiny::tt::PtrType::get(&Ctx, Float32Type::get(&Ctx),
                                  tiny::tt::PtrType::kGlobalAddressSpace);
  if (Spec.consume_front("ptr<") && Spec.consume_back(">")) {
    Type Pointee;
    if (Spec == "f32")
      Pointee = Float32Type::get(&Ctx);
    else if (Spec == "i32")
      Pointee = IntegerType::get(&Ctx, 32);
    else
      return createStringError(inconvertibleErrorCode(),
                               "unsupported pointee type '%s'", Spec.str().c_str());
    return tiny::tt::PtrType::get(&Ctx, Pointee,
                                  tiny::tt::PtrType::kGlobalAddressSpace);
  }
  return createStringError(inconvertibleErrorCode(),
                           "unsupported --arg-type '%s'", Spec.str().c_str());
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(
      argc, argv, "tiny-triton: Triton subset frontend for the tiny Triton "
                  "pipeline\n");

  if (Emit != "ttir") {
    errs() << "tiny-triton: only --emit ttir exists in chapter 2"
              " (ttgir arrives in chapter 4, llir in chapter 5)\n";
    return 2;
  }
  if (Input.empty() && AstJson.empty()) {
    errs() << "tiny-triton: expected <kernel.py> or --ast-json <file>\n";
    return 2;
  }

  MLIRContext Ctx;
  DialectRegistry Registry;
  Registry.insert<arith::ArithDialect, func::FuncDialect, scf::SCFDialect,
                  tiny::tt::TinyTTDialect>();
  Ctx.appendDialectRegistry(Registry);
  Ctx.loadAllAvailableDialects();

  auto AstOrErr = AstJson.empty()
                      ? PyAst::loadFromPython(Input, Python)
                      : PyAst::loadFromJson(AstJson);
  if (!AstOrErr)
    return reportError(AstOrErr.takeError());
  const PyAst &Ast = *AstOrErr;

  FrontendOptions Opts;
  Opts.Kernel = Kernel;
  for (StringRef Spec : ArgTypeSpecs) {
    auto [Name, TypeText] = Spec.split('=');
    if (Name.empty() || TypeText.empty()) {
      errs() << "tiny-triton: expected --arg-type NAME=TYPE, got '" << Spec
             << "'\n";
      return 2;
    }
    auto TypeOrErr = parseTypeSpec(Ctx, TypeText);
    if (!TypeOrErr)
      return reportError(TypeOrErr.takeError());
    Opts.ArgTypes[Name.str()] = *TypeOrErr;
  }

  auto ModuleOrErr = emitTTIR(Ctx, Ast, Opts);
  if (!ModuleOrErr)
    return reportError(ModuleOrErr.takeError());

  std::unique_ptr<raw_ostream> OS;
  if (Output == "-") {
    OS = std::make_unique<raw_fd_ostream>(1, /*shouldClose=*/false);
  } else {
    std::error_code EC;
    OS = std::make_unique<raw_fd_ostream>(Output, EC);
    if (EC) {
      errs() << "tiny-triton: cannot open '" << Output << "': " << EC.message()
             << "\n";
      return 1;
    }
  }
  (*ModuleOrErr)->print(*OS);
  OS->flush();
  return 0;
}
