//===-- main.cpp - tiny-triton driver -------------------------------------===//
//
//   tiny-triton kernel.py --emit ttir --arg-type a_ptr=ptr ...
//   tiny-triton kernel.py --emit ttgir|llir ...          (stages in order)
//   tiny-triton --input ttir  kernel.ttir  --emit ttgir
//   tiny-triton --input ttgir kernel.ttgir --emit llir
//   tiny-triton kernel.py --dump-ast
//   tiny-triton --ast-json kernel.ast.json --emit ttir ...
//
// Chapter 8 turns the driver into a stage runner.  Every stage boundary is an
// MLIR module, never text: the same pass objects `tiny-opt` registers are run
// here, so `--input ttir --emit ttgir` and
// `tiny-opt --tiny-convert-tt-to-ttg` are byte-identical by construction.
// Asking for LLIR straight from TTIR is refused: the pipeline has three
// checkpoints and skipping one is exactly what this course does not do.
//
//===----------------------------------------------------------------------===//

#include "EmitTTIR.h"
#include "PyAst.h"

#include "tiny/Conversion/TinyTTGToLLVM.h"
#include "tiny/Conversion/TinyTTToTinyTTG.h"
#include "tiny/Dialect/TinyTT/TinyTT.h"
#include "tiny/Dialect/TinyTTG/TinyTTGDialect.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include <memory>
#include <optional>

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
static cl::opt<std::string> Emit(
    "emit", cl::desc("Output stage: ttir (default), ttgir or llir"),
    cl::value_desc("stage"), cl::init("ttir"));
static cl::opt<std::string> InputStage(
    "input", cl::desc("Input stage of a .mlir file: ttir or ttgir"),
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
    mlir::Type Pointee;
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

namespace {

/// The three checkpoints of the pipeline, in order.
enum class Stage { TTIR, TTGIR, LLIR };

std::optional<Stage> parseStage(StringRef Text) {
  if (Text == "ttir")
    return Stage::TTIR;
  if (Text == "ttgir")
    return Stage::TTGIR;
  if (Text == "llir")
    return Stage::LLIR;
  return std::nullopt;
}

StringRef stageName(Stage S) {
  switch (S) {
  case Stage::TTIR:
    return "ttir";
  case Stage::TTGIR:
    return "ttgir";
  case Stage::LLIR:
    return "llir";
  }
  llvm_unreachable("unknown stage");
}

/// Run one registered pass over the module.  The pass objects are the ones
/// `tiny-opt` exposes, so both drivers produce the same IR.
LogicalResult runPass(ModuleOp module, Stage to) {
  PassManager manager(module.getContext());
  manager.enableVerifier(true);
  if (to == Stage::TTGIR)
    manager.addPass(tiny::createTinyTTToTinyTTGPass());
  else
    manager.addPass(tiny::createTinyTTGToLLVMPass());
  return manager.run(module);
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(
      argc, argv, "tiny-triton: Triton subset frontend for the tiny Triton "
                  "pipeline\n");

  if (Input.empty() && AstJson.empty()) {
    errs() << "tiny-triton: expected <kernel.py> or --ast-json <file>\n";
    return 2;
  }

  auto EmitStage = parseStage(Emit);
  if (!EmitStage) {
    errs() << "tiny-triton: --emit must be ttir, ttgir or llir\n";
    return 2;
  }

  std::optional<Stage> SourceStage;
  if (!InputStage.empty()) {
    SourceStage = parseStage(InputStage);
    if (!SourceStage || *SourceStage == Stage::LLIR) {
      errs() << "tiny-triton: --input must be ttir or ttgir\n";
      return 2;
    }
  }

  MLIRContext Ctx;
  DialectRegistry Registry;
  Registry.insert<arith::ArithDialect, func::FuncDialect, scf::SCFDialect,
                  LLVM::LLVMDialect, tiny::tt::TinyTTDialect,
                  tiny::ttg::TinyTTGDialect>();
  Ctx.appendDialectRegistry(Registry);
  Ctx.loadAllAvailableDialects();

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

  // A .mlir input starts the pipeline at the stage the file claims to be.
  if (SourceStage) {
    if (*SourceStage == Stage::TTIR && *EmitStage == Stage::LLIR) {
      errs() << "tiny-triton: cannot lower TTIR directly to LLIR; run "
                "--emit ttgir first\n";
      return 2;
    }
    if (*EmitStage < *SourceStage) {
      errs() << "tiny-triton: cannot go back from " << stageName(*SourceStage)
             << " to " << stageName(*EmitStage) << "\n";
      return 2;
    }

    OwningOpRef<ModuleOp> Module = parseSourceFile<ModuleOp>(Input, &Ctx);
    if (!Module)
      return 1;
    if (*EmitStage != *SourceStage) {
      if (failed(runPass(*Module, *EmitStage)))
        return 1;
    }
    Module->print(*OS);
    *OS << "\n";
    OS->flush();
    return 0;
  }

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

  // A Python kernel always enters at TTIR; later stages run in order, one
  // MLIR module in memory between each pair.
  ModuleOp Module = ModuleOrErr->get();
  if (*EmitStage != Stage::TTIR) {
    if (failed(runPass(Module, Stage::TTGIR)))
      return 1;
    if (*EmitStage == Stage::LLIR && failed(runPass(Module, Stage::LLIR)))
      return 1;
  }
  Module.print(*OS);
  *OS << "\n";
  OS->flush();
  return 0;
}
