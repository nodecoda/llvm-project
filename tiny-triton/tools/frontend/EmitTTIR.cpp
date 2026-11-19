//===-- EmitTTIR.cpp - AST -> tinytt lowering -----------------------------===//
//
// Chapter 2 supports straight-line kernels: assignments, `tl.program_id`,
// `tl.arange`, elementwise arithmetic, `tl.load` and `tl.store`.  Control
// flow, reductions and shared memory arrive in chapters 6 and 7.
//
// There is no private IR between the AST and `tinytt`: every Python value
// becomes an MLIR Value and every statement becomes MLIR ops.
//
//===----------------------------------------------------------------------===//

#include "EmitTTIR.h"

#include "tiny/Dialect/TinyTT/TinyTT.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Error.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"

using namespace llvm;
using namespace mlir;
using json::Object;
using JsonValue = json::Value;

namespace tinytriton {

namespace {

StringRef typeOf(const Object &Node) {
  if (auto S = Node.getString("_type"))
    return *S;
  return {};
}

StringRef stringField(const Object &Node, StringRef Name) {
  if (auto S = Node.getString(Name))
    return *S;
  return {};
}

unsigned lineOf(const Object &Node) {
  if (auto L = Node.getInteger("lineno"))
    return static_cast<unsigned>(*L);
  return 0;
}

const Object *asObject(const JsonValue &V) { return V.getAsObject(); }

const Object *fieldObject(const Object &Node, StringRef Name) {
  if (const JsonValue *V = Node.get(Name))
    return asObject(*V);
  return nullptr;
}

const json::Array *fieldArray(const Object &Node, StringRef Name) {
  if (const JsonValue *V = Node.get(Name))
    return V->getAsArray();
  return nullptr;
}

Error frontendError(unsigned Line, const Twine &Message) {
  return createStringError(inconvertibleErrorCode(), "line %u: %s", Line,
                           Message.str().c_str());
}

bool isJitDecorator(const JsonValue &Decorator) {
  const Object *Node = asObject(Decorator);
  if (!Node)
    return false;
  StringRef Type = typeOf(*Node);
  if (Type == "Name")
    return stringField(*Node, "id") == "jit";
  if (Type == "Attribute")
    return stringField(*Node, "attr") == "jit";
  if (Type == "Call")
    if (const JsonValue *Func = Node->get("func"))
      return isJitDecorator(*Func);
  return false;
}

bool isKernelDef(const Object &Node) {
  if (typeOf(Node) != "FunctionDef")
    return false;
  const json::Array *Decorators = fieldArray(Node, "decorator_list");
  if (!Decorators)
    return false;
  for (const JsonValue &Decorator : *Decorators)
    if (isJitDecorator(Decorator))
      return true;
  return false;
}

std::optional<int64_t> constantInt(const JsonValue &V) {
  const Object *Node = asObject(V);
  if (!Node || typeOf(*Node) != "Constant")
    return std::nullopt;
  const JsonValue *Value = Node->get("value");
  if (!Value)
    return std::nullopt;
  return Value->getAsInteger();
}

bool isTensorType(Type T) { return isa<RankedTensorType>(T); }

Type elementOf(Type T) {
  if (auto RT = dyn_cast<RankedTensorType>(T))
    return RT.getElementType();
  return T;
}

bool isPtrLike(Type T) { return isa<tiny::tt::PtrType>(elementOf(T)); }

bool isFloatOrInt(Type T) { return T.isIntOrIndex() || isa<FloatType>(T); }

} // namespace

//===----------------------------------------------------------------------===//
// Kernel discovery
//===----------------------------------------------------------------------===//

Expected<std::vector<KernelDecl>> parseKernels(const PyAst &Ast) {
  std::vector<KernelDecl> Kernels;
  const Object &Root = Ast.root();
  const json::Array *Body = fieldArray(Root, "body");
  if (!Body)
    return createStringError(inconvertibleErrorCode(), "AST has no module body");

  for (const JsonValue &Item : *Body) {
    const Object *Node = asObject(Item);
    if (!Node || !isKernelDef(*Node))
      continue;

    KernelDecl Decl;
    Decl.Name = stringField(*Node, "name").str();
    Decl.Line = lineOf(*Node);

    const Object *ArgsNode = fieldObject(*Node, "args");
    const json::Array *ArgList =
        ArgsNode ? fieldArray(*ArgsNode, "args") : nullptr;
    if (ArgList)
      for (const JsonValue &ArgValue : *ArgList) {
        const Object *ArgObj = asObject(ArgValue);
        if (!ArgObj)
          continue;
        KernelArg Arg;
        Arg.Name = stringField(*ArgObj, "arg").str();
        Arg.Line = lineOf(*ArgObj);
        Decl.Args.push_back(std::move(Arg));
      }
    Kernels.push_back(std::move(Decl));
  }
  return Kernels;
}

//===----------------------------------------------------------------------===//
// Lowering
//===----------------------------------------------------------------------===//

namespace {

class Emitter {
public:
  Emitter(MLIRContext &Ctx, const KernelDecl &Decl,
          const StringMap<Type> &ArgTypes, StringRef SourcePath)
      : Ctx(Ctx), Decl(Decl), ArgTypes(ArgTypes), Builder(&Ctx),
        SourcePath(SourcePath) {}

  Expected<OwningOpRef<ModuleOp>> run(const Object &FnNode);

private:
  Location loc(unsigned Line) {
    return FileLineColLoc::get(&Ctx, SourcePath, Line, 0u);
  }

  Expected<Value> lowerExpr(const JsonValue &Node);
  Expected<Value> lowerCall(const Object &Call, unsigned Line);
  Expected<Value> lowerBinary(StringRef OpName, Value Lhs, Value Rhs,
                              unsigned Line);
  Expected<Value> lowerUnary(StringRef OpName, const JsonValue &Operand,
                             unsigned Line);
  Expected<Value> splatTo(Value Scalar, RankedTensorType Target, unsigned Line);
  Expected<RankedTensorType> broadcastShape(RankedTensorType Lhs,
                                            RankedTensorType Rhs,
                                            unsigned Line);
  Expected<Value> broadcastTo(Value Value, RankedTensorType Target,
                              unsigned Line);
  Expected<Value> lowerSubscript(const Object &Node, unsigned Line);
  Error lowerStmt(const Object &Stmt);
  Expected<Value> lowerConstant(const Object &ConstNode, unsigned Line);
  Expected<Value> lowerAttribute(const Object &AttrNode, unsigned Line);

  MLIRContext &Ctx;
  const KernelDecl &Decl;
  const StringMap<Type> &ArgTypes;
  OpBuilder Builder;
  StringMap<Value> Env;
  StringRef SourcePath;
};

Expected<Value> Emitter::lowerConstant(const Object &Node, unsigned Line) {
  const JsonValue *Val = Node.get("value");
  if (!Val)
    return frontendError(Line, "malformed Constant");
  bool IsFloat = Node.getBoolean("value_is_float").value_or(false);

  if (IsFloat) {
    double D = 0.0;
    if (auto AsDouble = Val->getAsNumber())
      D = *AsDouble;
    return Builder
        .create<arith::ConstantOp>(
            loc(Line),
            Builder.getF32FloatAttr(static_cast<float>(D)))
        .getResult();
  }

  if (auto AsInt = Val->getAsInteger())
    return Builder
        .create<arith::ConstantOp>(
            loc(Line),
            Builder.getI32IntegerAttr(static_cast<int32_t>(*AsInt)))
        .getResult();

  if (Val->getAsNull())
    return frontendError(Line, "None constants are not supported yet");

  return frontendError(Line, "unsupported constant kind");
}

Expected<Value> Emitter::lowerAttribute(const Object &Node, unsigned Line) {
  StringRef Attr = stringField(Node, "attr");
  const Object *Base = fieldObject(Node, "value");
  StringRef BaseName = Base ? stringField(*Base, "id") : StringRef();
  return frontendError(Line, "'" + BaseName + "." + Attr +
                                 "' is only valid as a call target");
}

Expected<Value> Emitter::splatTo(Value Scalar, RankedTensorType Target,
                                 unsigned Line) {
  if (isTensorType(Scalar.getType()))
    return frontendError(Line, "cannot splat a tensor");
  if (Scalar.getType() != Target.getElementType())
    return frontendError(Line, "splat element type mismatch");
  return Builder.create<tiny::tt::SplatOp>(loc(Line), Target, Scalar)
      .getResult();
}

Expected<RankedTensorType> Emitter::broadcastShape(RankedTensorType Lhs,
                                                   RankedTensorType Rhs,
                                                   unsigned Line) {
  if (Lhs.getRank() != Rhs.getRank())
    return frontendError(Line, "cannot broadcast tensors of rank " +
                                   Twine(Lhs.getRank()) + " and " +
                                   Twine(Rhs.getRank()));
  if (Lhs.getElementType() != Rhs.getElementType())
    return frontendError(Line, "tensor element types differ");
  SmallVector<int64_t> Shape;
  Shape.reserve(Lhs.getRank());
  for (int64_t Dim = 0; Dim < Lhs.getRank(); ++Dim) {
    int64_t L = Lhs.getShape()[Dim];
    int64_t R = Rhs.getShape()[Dim];
    if (L == R)
      Shape.push_back(L);
    else if (L == 1)
      Shape.push_back(R);
    else if (R == 1)
      Shape.push_back(L);
    else
      return frontendError(Line, "cannot broadcast dimension " + Twine(Dim) +
                                     ": " + Twine(L) + " against " +
                                     Twine(R));
  }
  return RankedTensorType::get(Shape, Lhs.getElementType());
}

Expected<Value> Emitter::broadcastTo(Value Value, RankedTensorType Target,
                                     unsigned Line) {
  auto Source = dyn_cast<RankedTensorType>(Value.getType());
  if (!Source)
    return frontendError(Line, "broadcast needs a ranked tensor");
  if (Source.getShape() == Target.getShape())
    return Value;
  return Builder.create<tiny::tt::BroadcastOp>(loc(Line), Target, Value)
      .getResult();
}

Expected<Value> Emitter::lowerBinary(StringRef OpName, Value Lhs, Value Rhs,
                                     unsigned Line) {
  Type LhsTy = Lhs.getType();
  Type RhsTy = Rhs.getType();

  if (isPtrLike(LhsTy) || isPtrLike(RhsTy)) {
    if (!isPtrLike(LhsTy))
      return frontendError(Line,
                           "pointer arithmetic needs the pointer on the left");
    if (OpName != "Add")
      return frontendError(Line, "only `ptr + offsets` is supported");

    if (isTensorType(LhsTy) && isTensorType(RhsTy)) {
      auto LhsTensor = cast<RankedTensorType>(LhsTy);
      auto RhsTensor = cast<RankedTensorType>(RhsTy);
      if (LhsTensor.getShape() != RhsTensor.getShape()) {
        // The pointer and its offset may differ in a size-1 dimension: an
        // 8x8 tile is addressed with a row vector and a column vector.
        Expected<RankedTensorType> Common =
            broadcastShape(LhsTensor, RhsTensor, Line);
        if (!Common)
          return Common.takeError();
        auto PtrTarget =
            RankedTensorType::get(Common->getShape(), LhsTensor.getElementType());
        auto OffTarget =
            RankedTensorType::get(Common->getShape(), RhsTensor.getElementType());
        Expected<Value> BroadcastPtr = broadcastTo(Lhs, PtrTarget, Line);
        if (!BroadcastPtr)
          return BroadcastPtr.takeError();
        Expected<Value> BroadcastOff = broadcastTo(Rhs, OffTarget, Line);
        if (!BroadcastOff)
          return BroadcastOff.takeError();
        Lhs = *BroadcastPtr;
        Rhs = *BroadcastOff;
      }
    } else if (isTensorType(LhsTy)) {
      // tensor<ptr> + scalar offset
      auto Shape = cast<RankedTensorType>(LhsTy).getShape();
      auto Target = RankedTensorType::get(Shape, Rhs.getType());
      auto Splatted = splatTo(Rhs, Target, Line);
      if (!Splatted)
        return Splatted.takeError();
      Rhs = *Splatted;
    } else if (isTensorType(RhsTy)) {
      // scalar ptr + tensor<offset>: broadcast the pointer to the offset shape
      auto Shape = cast<RankedTensorType>(RhsTy).getShape();
      auto Target = RankedTensorType::get(Shape, Lhs.getType());
      auto Splatted = splatTo(Lhs, Target, Line);
      if (!Splatted)
        return Splatted.takeError();
      Lhs = *Splatted;
    } else {
      return frontendError(Line, "pointer arithmetic needs a tensor offset");
    }
    return Builder.create<tiny::tt::AddPtrOp>(loc(Line), Lhs.getType(), Lhs, Rhs)
        .getResult();
  }

  if (isTensorType(LhsTy) != isTensorType(RhsTy)) {
    if (isTensorType(LhsTy)) {
      auto Splatted = splatTo(Rhs, cast<RankedTensorType>(LhsTy), Line);
      if (!Splatted)
        return Splatted.takeError();
      Rhs = *Splatted;
    } else {
      auto Splatted = splatTo(Lhs, cast<RankedTensorType>(RhsTy), Line);
      if (!Splatted)
        return Splatted.takeError();
      Lhs = *Splatted;
    }
    LhsTy = Lhs.getType();
    RhsTy = Rhs.getType();
  }

  if (isTensorType(LhsTy)) {
    auto LhsTensor = cast<RankedTensorType>(LhsTy);
    auto RhsTensor = cast<RankedTensorType>(RhsTy);
    if (LhsTensor.getShape() != RhsTensor.getShape()) {
      // `r * 8 + c` with r : tensor<8x1> and c : tensor<1x8> is the chapter 7
      // spelling of an 8x8 index tile.  Both sides are grown to the common
      // shape first, so the arithmetic itself stays elementwise.
      Expected<RankedTensorType> Common =
          broadcastShape(LhsTensor, RhsTensor, Line);
      if (!Common)
        return Common.takeError();
      Expected<Value> BroadcastLhs = broadcastTo(Lhs, *Common, Line);
      if (!BroadcastLhs)
        return BroadcastLhs.takeError();
      Expected<Value> BroadcastRhs = broadcastTo(Rhs, *Common, Line);
      if (!BroadcastRhs)
        return BroadcastRhs.takeError();
      Lhs = *BroadcastLhs;
      Rhs = *BroadcastRhs;
      LhsTy = Lhs.getType();
      RhsTy = Rhs.getType();
    }
    if (LhsTensor.getElementType() != RhsTensor.getElementType())
      return frontendError(Line, "tensor element types differ");
  }

  Type ElementType = elementOf(LhsTy);
  if (!isFloatOrInt(ElementType))
    return frontendError(Line, "arithmetic on a non-numeric type");
  bool IsFloat = isa<FloatType>(ElementType);

  if (OpName == "Add")
    return IsFloat
               ? Builder.create<arith::AddFOp>(loc(Line), Lhs, Rhs).getResult()
               : Builder.create<arith::AddIOp>(loc(Line), Lhs, Rhs).getResult();
  if (OpName == "Sub")
    return IsFloat
               ? Builder.create<arith::SubFOp>(loc(Line), Lhs, Rhs).getResult()
               : Builder.create<arith::SubIOp>(loc(Line), Lhs, Rhs).getResult();
  if (OpName == "Mult")
    return IsFloat
               ? Builder.create<arith::MulFOp>(loc(Line), Lhs, Rhs).getResult()
               : Builder.create<arith::MulIOp>(loc(Line), Lhs, Rhs).getResult();
  if (OpName == "Div") {
    if (!IsFloat)
      return frontendError(Line, "integer division is not supported yet");
    return Builder.create<arith::DivFOp>(loc(Line), Lhs, Rhs).getResult();
  }
  return frontendError(Line, "unsupported binary operator '" + OpName + "'");
}

Expected<Value> Emitter::lowerUnary(StringRef OpName, const JsonValue &Operand,
                                    unsigned Line) {
  if (OpName != "USub")
    return frontendError(Line, "unsupported unary operator '" + OpName + "'");

  auto Lowered = lowerExpr(Operand);
  if (!Lowered)
    return Lowered.takeError();
  Value V = *Lowered;
  Type Ty = V.getType();
  if (!isFloatOrInt(elementOf(Ty)))
    return frontendError(Line, "cannot negate a non-numeric value");

  Attribute ZeroAttr = Builder.getZeroAttr(Ty);
  if (!ZeroAttr)
    return frontendError(Line, "cannot negate this type");
  Value Zero = Builder.create<arith::ConstantOp>(
      loc(Line), cast<TypedAttr>(ZeroAttr));
  return lowerBinary("Sub", Zero, V, Line);
}

Expected<Value> Emitter::lowerCall(const Object &Call, unsigned Line) {
  const Object *Func = fieldObject(Call, "func");
  if (!Func || typeOf(*Func) != "Attribute")
    return frontendError(Line, "only `tl.<name>(...)` calls are supported");

  const Object *Base = fieldObject(*Func, "value");
  StringRef BaseName = Base ? stringField(*Base, "id") : StringRef();
  StringRef Attr = stringField(*Func, "attr");
  if (BaseName != "tl")
    return frontendError(Line, "only `tl.<name>(...)` calls are supported");

  const json::Array *RawArgs = fieldArray(Call, "args");

  if (Attr == "arange") {
    if (!RawArgs || RawArgs->size() != 2)
      return frontendError(Line, "tl.arange(start, end) takes two constants");
    auto Start = constantInt((*RawArgs)[0]);
    auto End = constantInt((*RawArgs)[1]);
    if (!Start || !End)
      return frontendError(Line, "tl.arange bounds must be constants");
    if (*Start != 0)
      return frontendError(Line, "tl.arange must start at 0 in chapter 2");
    int64_t Count = *End - *Start;
    if (Count <= 0)
      return frontendError(Line, "tl.arange needs a positive length");
    auto ResultTy = RankedTensorType::get({Count}, Builder.getI32Type());
    return Builder
        .create<tiny::tt::MakeRangeOp>(loc(Line), ResultTy,
                                       Builder.getI32IntegerAttr(0))
        .getResult();
  }

  if (Attr == "program_id") {
    if (!RawArgs || RawArgs->size() != 1)
      return frontendError(Line, "tl.program_id expects one axis");
    auto Axis = constantInt((*RawArgs)[0]);
    if (!Axis)
      return frontendError(Line, "tl.program_id axis must be a constant");
    if (*Axis < 0 || *Axis > 2)
      return frontendError(Line, "tl.program_id axis must be 0, 1 or 2");
    return Builder
        .create<tiny::tt::GetProgramIdOp>(
            loc(Line), Builder.getI32Type(),
            Builder.getI32IntegerAttr(static_cast<int32_t>(*Axis)))
        .getResult();
  }

  SmallVector<Value, 4> Args;
  if (RawArgs)
    for (const JsonValue &ArgValue : *RawArgs) {
      auto LoweredArg = lowerExpr(ArgValue);
      if (!LoweredArg)
        return LoweredArg.takeError();
      Args.push_back(*LoweredArg);
    }

  if (Attr == "sum") {
    if (Args.empty() || Args.size() > 2)
      return frontendError(Line, "tl.sum takes a tensor and an optional axis");
    auto InputTy = dyn_cast<RankedTensorType>(Args[0].getType());
    if (!InputTy || InputTy.getRank() != 1)
      return frontendError(Line, "tiny only reduces 1-D tensors");
    int64_t Axis = 0;
    if (Args.size() == 2) {
      auto Const = constantInt((*RawArgs)[1]);
      if (!Const)
        return frontendError(Line, "tl.sum axis must be a constant");
      Axis = *Const;
    }
    if (Axis != 0)
      return frontendError(Line, "tiny only reduces axis 0");
    return Builder
        .create<tiny::tt::ReduceOp>(loc(Line), InputTy.getElementType(),
                                    Args[0], Builder.getI32IntegerAttr(0),
                                    Builder.getStringAttr("add"))
        .getResult();
  }

  if (Attr == "load") {
    if (Args.empty() || Args.size() > 3)
      return frontendError(Line, "tl.load takes 1 or 3 arguments");
    auto PtrTy = dyn_cast<RankedTensorType>(Args[0].getType());
    if (!PtrTy)
      return frontendError(Line, "tl.load needs a tensor of pointers");
    auto Pointee = dyn_cast<tiny::tt::PtrType>(PtrTy.getElementType());
    if (!Pointee)
      return frontendError(Line, "tl.load needs a tensor of pointers");
    auto ResultTy =
        RankedTensorType::get(PtrTy.getShape(), Pointee.getPointee());
    if (Args.size() == 1)
      return Builder.create<tiny::tt::LoadOp>(loc(Line), ResultTy, Args[0])
          .getResult();
    if (Args.size() == 3)
      return Builder
          .create<tiny::tt::MaskedLoadOp>(loc(Line), ResultTy, Args[0], Args[1],
                                          Args[2])
          .getResult();
    return frontendError(Line,
                         "tl.load with only a mask is not supported yet");
  }

  if (Attr == "store") {
    if (Args.size() != 2 && Args.size() != 3)
      return frontendError(Line, "tl.store takes 2 or 3 arguments");
    // A reduction produces a scalar, so `tl.store(out_ptr, sum)` is a scalar
    // store rather than a distributed one.
    if (isa<tiny::tt::PtrType>(Args[0].getType())) {
      if (Args.size() != 2)
        return frontendError(Line, "a scalar store takes exactly 2 arguments");
      Builder.create<tiny::tt::StoreScalarOp>(loc(Line), Args[0], Args[1]);
      return mlir::Value();
    }
    if (Args.size() == 2)
      Builder.create<tiny::tt::StoreOp>(loc(Line), Args[0], Args[1]);
    else
      Builder.create<tiny::tt::MaskedStoreOp>(loc(Line), Args[0], Args[1],
                                              Args[2]);
    return mlir::Value();
  }

  return frontendError(Line, "unsupported tl API '" + Attr +
                                 "' (chapter 2 knows program_id, arange, "
                                 "load, store; chapter 6 adds sum)");
}

Expected<Value> Emitter::lowerSubscript(const Object &Node, unsigned Line) {
  const JsonValue *ValueNode = Node.get("value");
  const JsonValue *SliceNode = Node.get("slice");
  if (!ValueNode || !SliceNode)
    return frontendError(Line, "malformed Subscript");

  auto Lowered = lowerExpr(*ValueNode);
  if (!Lowered)
    return Lowered.takeError();
  auto TensorTy = dyn_cast<RankedTensorType>((*Lowered).getType());
  if (!TensorTy || TensorTy.getRank() != 1)
    return frontendError(Line,
                         "chapter 7 only subscripts a 1-D tensor, as in "
                         "tl.arange(0, 8)[:, None]");

  // `x[:, None]` and `x[None, :]` are the only accepted forms: they insert a
  // size-1 dimension, which is exactly `tinytt.expand_dims`.
  auto IsNone = [](const JsonValue &V) {
    const Object *O = asObject(V);
    if (!O || typeOf(*O) != "Constant")
      return false;
    const JsonValue *Val = O->get("value");
    return Val && Val->getAsNull();
  };
  auto IsFullSlice = [](const JsonValue &V) {
    const Object *O = asObject(V);
    if (!O || typeOf(*O) != "Slice")
      return false;
    for (const char *Field : {"lower", "upper", "step"}) {
      const JsonValue *F = O->get(Field);
      if (F && !F->getAsNull())
        return false;
    }
    return true;
  };

  const json::Array *Elts = nullptr;
  if (const Object *SliceObj = asObject(*SliceNode))
    if (typeOf(*SliceObj) == "Tuple")
      Elts = fieldArray(*SliceObj, "elts");
  if (!Elts || Elts->size() != 2)
    return frontendError(Line, "chapter 7 accepts x[:, None] and x[None, :]");

  int64_t Axis = 0;
  if (IsFullSlice((*Elts)[0]) && IsNone((*Elts)[1]))
    Axis = 1;
  else if (IsNone((*Elts)[0]) && IsFullSlice((*Elts)[1]))
    Axis = 0;
  else
    return frontendError(Line, "chapter 7 accepts x[:, None] and x[None, :]");

  SmallVector<int64_t> Shape(TensorTy.getShape().begin(),
                             TensorTy.getShape().end());
  Shape.insert(Shape.begin() + Axis, 1);
  auto ResultTy = RankedTensorType::get(Shape, TensorTy.getElementType());
  return Builder
      .create<tiny::tt::ExpandDimsOp>(loc(Line), ResultTy, *Lowered,
                                      Builder.getI32IntegerAttr(Axis))
      .getResult();
}

Expected<Value> Emitter::lowerExpr(const JsonValue &Node) {
  const Object *N = asObject(Node);
  if (!N)
    return frontendError(0, "expected an AST node");

  StringRef Type = typeOf(*N);
  unsigned Line = lineOf(*N);

  if (Type == "Constant")
    return lowerConstant(*N, Line);
  if (Type == "Name") {
    StringRef Id = stringField(*N, "id");
    if (Id == "tl" || Id == "triton")
      return frontendError(Line, "'" + Id + "' is a module, not a value");
    auto It = Env.find(Id);
    if (It == Env.end())
      return frontendError(Line, "unknown name '" + Id + "'");
    return It->second;
  }
  if (Type == "Attribute")
    return lowerAttribute(*N, Line);
  if (Type == "BinOp") {
    const Object *Op = fieldObject(*N, "op");
    const JsonValue *Left = N->get("left");
    const JsonValue *Right = N->get("right");
    if (!Op || !Left || !Right)
      return frontendError(Line, "malformed BinOp");
    auto Lhs = lowerExpr(*Left);
    if (!Lhs)
      return Lhs.takeError();
    auto Rhs = lowerExpr(*Right);
    if (!Rhs)
      return Rhs.takeError();
    return lowerBinary(typeOf(*Op), *Lhs, *Rhs, Line);
  }
  if (Type == "UnaryOp") {
    const Object *Op = fieldObject(*N, "op");
    const JsonValue *Operand = N->get("operand");
    if (!Op || !Operand)
      return frontendError(Line, "malformed UnaryOp");
    return lowerUnary(typeOf(*Op), *Operand, Line);
  }
  if (Type == "Call")
    return lowerCall(*N, Line);
  if (Type == "Subscript")
    return lowerSubscript(*N, Line);

  return frontendError(Line, "unsupported expression '" + Type +
                                 "' (chapter 2 handles straight-line code)");
}

Error Emitter::lowerStmt(const Object &Stmt) {
  StringRef Type = typeOf(Stmt);
  unsigned Line = lineOf(Stmt);

  if (Type == "Assign") {
    const JsonValue *ValNode = Stmt.get("value");
    if (!ValNode)
      return frontendError(Line, "malformed Assign");
    auto Lowered = lowerExpr(*ValNode);
    if (!Lowered)
      return Lowered.takeError();

    const json::Array *Targets = fieldArray(Stmt, "targets");
    if (!Targets || Targets->empty())
      return frontendError(Line, "assignment without a target");
    for (const JsonValue &Target : *Targets) {
      const Object *TargetObj = asObject(Target);
      if (!TargetObj || typeOf(*TargetObj) != "Name")
        return frontendError(Line, "chapter 2 only assigns to plain names");
      Env[stringField(*TargetObj, "id")] = *Lowered;
    }
    return Error::success();
  }

  if (Type == "Expr") {
    const JsonValue *ValNode = Stmt.get("value");
    if (!ValNode)
      return frontendError(Line, "malformed Expr");
    auto Lowered = lowerExpr(*ValNode);
    if (!Lowered)
      return Lowered.takeError();
    return Error::success();
  }

  if (Type == "Return") {
    Builder.create<func::ReturnOp>(loc(Line));
    return Error::success();
  }

  if (Type == "Pass")
    return Error::success();

  if (Type == "If" || Type == "For" || Type == "While")
    return frontendError(Line, "control flow arrives in chapter 6; chapter 2 "
                               "handles straight-line kernels only");

  return frontendError(Line, "unsupported statement '" + Type + "'");
}

Expected<OwningOpRef<ModuleOp>> Emitter::run(const Object &FnNode) {
  auto Module = ModuleOp::create(loc(Decl.Line));
  Builder.setInsertionPointToEnd(Module.getBody());

  SmallVector<Type> ArgTys;
  ArgTys.reserve(Decl.Args.size());
  for (const KernelArg &Arg : Decl.Args) {
    auto It = ArgTypes.find(Arg.Name);
    if (It == ArgTypes.end())
      return frontendError(Arg.Line,
                           "missing --arg-type for '" + StringRef(Arg.Name) + "'");
    ArgTys.push_back(It->second);
  }

  auto FnTy = Builder.getFunctionType(ArgTys, {});
  auto Fn = func::FuncOp::create(loc(Decl.Line), Decl.Name, FnTy);
  Module.push_back(Fn);

  Block *Entry = Fn.addEntryBlock();
  Builder.setInsertionPointToEnd(Entry);
  for (unsigned I = 0; I < Decl.Args.size(); ++I)
    Env[Decl.Args[I].Name] = Entry->getArgument(I);

  const json::Array *Body = fieldArray(FnNode, "body");
  if (!Body)
    return frontendError(Decl.Line, "kernel has no body");
  for (const JsonValue &Stmt : *Body) {
    const Object *StmtObj = asObject(Stmt);
    if (!StmtObj)
      return frontendError(Decl.Line, "malformed statement");
    if (Error E = lowerStmt(*StmtObj))
      return std::move(E);
  }

  bool HasTerminator =
      !Entry->empty() && Entry->back().hasTrait<OpTrait::IsTerminator>();
  if (!HasTerminator)
    Builder.create<func::ReturnOp>(loc(Decl.Line));

  return OwningOpRef<ModuleOp>(Module);
}

} // namespace

Expected<OwningOpRef<ModuleOp>> emitTTIR(MLIRContext &Ctx, const PyAst &Ast,
                                         const FrontendOptions &Opts) {
  auto Kernels = parseKernels(Ast);
  if (!Kernels)
    return Kernels.takeError();
  if (Kernels->empty())
    return createStringError(inconvertibleErrorCode(),
                             "no @triton.jit kernel found in '%s'",
                             Ast.sourcePath().str().c_str());

  const KernelDecl *Selected = nullptr;
  if (Opts.Kernel.empty()) {
    Selected = &Kernels->front();
  } else {
    for (const KernelDecl &Decl : *Kernels)
      if (Decl.Name == Opts.Kernel)
        Selected = &Decl;
    if (!Selected)
      return createStringError(inconvertibleErrorCode(), "no kernel named '%s'",
                               Opts.Kernel.c_str());
  }

  const Object &Root = Ast.root();
  const json::Array *Body = fieldArray(Root, "body");
  const Object *FnNode = nullptr;
  for (const JsonValue &Item : *Body) {
    const Object *Node = asObject(Item);
    if (Node && isKernelDef(*Node) &&
        stringField(*Node, "name") == Selected->Name)
      FnNode = Node;
  }
  if (!FnNode)
    return createStringError(inconvertibleErrorCode(),
                             "kernel '%s' disappeared from the AST",
                             Selected->Name.c_str());

  Emitter E(Ctx, *Selected, Opts.ArgTypes, Ast.sourcePath());
  return E.run(*FnNode);
}

} // namespace tinytriton
