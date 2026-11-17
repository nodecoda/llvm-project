//===-- RVMinAsmParser.cpp - Parse RVMin assembly to MCInst ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A small hand-written parser for the RV32I subset RVMin implements.
//
// The parser only recognizes three operand shapes:
//
//   register      : a0, zero, x5, fp, ...
//   expression    : 42, -4, 0x10, label, label+4
//   memory        : expression '(' register ')'   e.g. 8(sp)
//
// Everything else (mnemonic -> instruction, operand-range checks) is handled by
// the TableGen-generated matcher.  Having a real assembler in the target is
// what lets chapter 2 test the encoder against llvm-mc without waiting for
// instruction selection.
//
//===----------------------------------------------------------------------===//

#include "RVMinBaseInfo.h"
#include "MCTargetDesc/RVMinMCTargetDesc.h"
#include "TargetInfo/RVMinTargetInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCParser/MCParsedAsmOperand.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

// Defined by the generated matcher at the bottom of this file; declared here
// so `matchAndEmitInstruction` can use it for feature diagnostics.
static const char *getSubtargetFeatureName(uint64_t Val);

namespace {

class RVMinAsmParser : public MCTargetAsmParser {
  MCAsmParser &Parser;

  MCAsmParser &getParser() const { return Parser; }
  AsmLexer &getLexer() const { return Parser.getLexer(); }
  SMLoc getLoc() const { return getLexer().getLoc(); }

  bool matchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                               OperandVector &Operands, MCStreamer &Out,
                               uint64_t &ErrorInfo,
                               bool MatchingInlineAsm) override;

  bool parseRegister(MCRegister &Reg, SMLoc &StartLoc,
                     SMLoc &EndLoc) override;
  ParseStatus tryParseRegister(MCRegister &Reg, SMLoc &StartLoc,
                               SMLoc &EndLoc) override;

  bool parseInstruction(ParseInstructionInfo &Info, StringRef Name,
                        SMLoc NameLoc, OperandVector &Operands) override;

  ParseStatus parseDirective(AsmToken DirectiveID) override;
  ParseStatus parseLiteralValues(unsigned Size);
  bool parseOperand(OperandVector &Operands, StringRef Mnemonic);
  ParseStatus parseRegister(OperandVector &Operands);
  ParseStatus parseExpression(OperandVector &Operands);
  ParseStatus parseSpecifierOperand(OperandVector &Operands);
  ParseStatus parseMemOpBaseReg(OperandVector &Operands);

  /// @name Auto-generated Matcher Functions
  /// {

#define GET_ASSEMBLER_HEADER
#include "RVMinGenAsmMatcher.inc"

  /// }

public:
  RVMinAsmParser(const MCSubtargetInfo &STI, MCAsmParser &Parser,
                 const MCInstrInfo &MII)
      : MCTargetAsmParser(STI, MII), Parser(Parser) {
    MCAsmParserExtension::Initialize(Parser);
    setAvailableFeatures(ComputeAvailableFeatures(STI.getFeatureBits()));
  }
};

/// A parsed RVMin assembly operand.
class RVMinOperand : public MCParsedAsmOperand {
  enum KindTy { Token, Register, Immediate } Kind;

  SMLoc StartLoc, EndLoc;
  union {
    StringRef Tok;
    MCRegister Reg;
    const MCExpr *Imm;
  };

public:
  RVMinOperand(KindTy Kind) : MCParsedAsmOperand(), Kind(Kind) {}

  bool isToken() const override { return Kind == Token; }
  bool isReg() const override { return Kind == Register; }
  bool isImm() const override { return Kind == Immediate; }
  bool isMem() const override { return false; }

  StringRef getToken() const {
    assert(Kind == Token && "not a token");
    return Tok;
  }

  MCRegister getReg() const override {
    assert(Kind == Register && "not a register");
    return Reg;
  }

  const MCExpr *getImm() const {
    assert(Kind == Immediate && "not an immediate");
    return Imm;
  }

  SMLoc getStartLoc() const override { return StartLoc; }
  SMLoc getEndLoc() const override { return EndLoc; }

  void print(raw_ostream &O, const MCAsmInfo &MAI) const override {
    switch (Kind) {
    case Token:
      O << "Token " << Tok;
      break;
    case Register:
      O << "Register " << Reg.id();
      break;
    case Immediate:
      O << "Immediate ";
      MAI.printExpr(O, *Imm);
      break;
    }
  }

  bool evaluateConstantImm(int64_t &Value) const {
    return Kind == Immediate && Imm->evaluateAsAbsolute(Value);
  }

  // Predicates referenced from RVMinInstrInfo.td (PredicateMethod).
  bool isSImm12() const {
    int64_t Value;
    if (evaluateConstantImm(Value))
      return isInt<12>(Value);
    // %lo(sym) is resolved by a relocation, not at assembly time.
    return Kind == Immediate && isa<MCSpecifierExpr>(Imm);
  }
  bool isUImm5() const {
    int64_t Value;
    return evaluateConstantImm(Value) && isUInt<5>(Value);
  }
  bool isUImm20() const {
    int64_t Value;
    if (evaluateConstantImm(Value))
      return isUInt<20>(Value);
    // %hi(sym): the upper half of a symbolic address.
    return Kind == Immediate && isa<MCSpecifierExpr>(Imm);
  }

  // Branch and jump targets may still be unresolved symbols; accept those and
  // let the fixup machinery check the final value once layout is known.
  bool isSImm13Lsb0() const {
    if (Kind != Immediate)
      return false;
    int64_t Value;
    if (Imm->evaluateAsAbsolute(Value))
      return isShiftedInt<12, 1>(Value);
    return isa<MCSymbolRefExpr>(Imm) || isa<MCSpecifierExpr>(Imm);
  }
  bool isSImm21Lsb0() const {
    if (Kind != Immediate)
      return false;
    int64_t Value;
    if (Imm->evaluateAsAbsolute(Value))
      return isShiftedInt<20, 1>(Value);
    return isa<MCSymbolRefExpr>(Imm) || isa<MCSpecifierExpr>(Imm);
  }

  // Chapter 43: compressed-immediate predicates.
  bool isCLUIImm() const {
    int64_t Value;
    if (!evaluateConstantImm(Value))
      return false;
    // Accept both spellings: the 20-bit field (1048575) and the
    // sign-extended value the DAG carries (-1).
    if (Value < 0)
      return isInt<6>(Value) && Value != 0;
    return Value != 0 &&
           (isUInt<5>(Value) || (Value >= 0xfffe0 && Value <= 0xfffff));
  }
  bool isSImm6() const {
    int64_t Value;
    return evaluateConstantImm(Value) && isInt<6>(Value);
  }
  bool isUImm7Lsb00() const {
    int64_t Value;
    return evaluateConstantImm(Value) && isShiftedUInt<5, 2>(Value);
  }
  bool isUImm8Lsb00() const {
    int64_t Value;
    return evaluateConstantImm(Value) && isShiftedUInt<6, 2>(Value);
  }
  bool isUImm8Lsb000() const {
    int64_t Value;
    return evaluateConstantImm(Value) && isShiftedUInt<5, 3>(Value);
  }
  bool isUImm9Lsb000() const {
    int64_t Value;
    return evaluateConstantImm(Value) && isShiftedUInt<6, 3>(Value);
  }
  bool isUImm10Lsb00NonZero() const {
    int64_t Value;
    return evaluateConstantImm(Value) && Value != 0 &&
           isShiftedUInt<8, 2>(Value);
  }
  bool isSImm10Lsb0000NonZero() const {
    int64_t Value;
    return evaluateConstantImm(Value) && Value != 0 &&
           isShiftedInt<6, 4>(Value);
  }
  bool isSImm9Lsb0() const {
    if (Kind != Immediate)
      return false;
    int64_t Value;
    if (Imm->evaluateAsAbsolute(Value))
      return isShiftedInt<8, 1>(Value);
    return isa<MCSymbolRefExpr>(Imm) || isa<MCSpecifierExpr>(Imm);
  }
  bool isSImm12Lsb0() const {
    if (Kind != Immediate)
      return false;
    int64_t Value;
    if (Imm->evaluateAsAbsolute(Value))
      return isShiftedInt<11, 1>(Value);
    return isa<MCSymbolRefExpr>(Imm) || isa<MCSpecifierExpr>(Imm);
  }

  void addRegOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "invalid number of operands");
    Inst.addOperand(MCOperand::createReg(Reg));
  }

  void addImmOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "invalid number of operands");
    int64_t Value;
    if (Imm->evaluateAsAbsolute(Value))
      Inst.addOperand(MCOperand::createImm(Value));
    else
      Inst.addOperand(MCOperand::createExpr(Imm));
  }

  static std::unique_ptr<RVMinOperand> createToken(StringRef Tok, SMLoc Loc) {
    auto Op = std::make_unique<RVMinOperand>(Token);
    Op->Tok = Tok;
    Op->StartLoc = Loc;
    Op->EndLoc = Loc;
    return Op;
  }
  static std::unique_ptr<RVMinOperand> createReg(MCRegister Reg, SMLoc Start,
                                                 SMLoc End) {
    auto Op = std::make_unique<RVMinOperand>(Register);
    Op->Reg = Reg;
    Op->StartLoc = Start;
    Op->EndLoc = End;
    return Op;
  }
  static std::unique_ptr<RVMinOperand> createImm(const MCExpr *Expr, SMLoc Start,
                                                 SMLoc End) {
    auto Op = std::make_unique<RVMinOperand>(Immediate);
    Op->Imm = Expr;
    Op->StartLoc = Start;
    Op->EndLoc = End;
    return Op;
  }
};

} // end anonymous namespace

// Auto-generated by TableGen.
static MCRegister MatchRegisterName(StringRef Name);
static MCRegister MatchRegisterAltName(StringRef Name);

static MCRegister matchRegister(StringRef Name) {
  MCRegister Reg = MatchRegisterName(Name);
  if (Reg == RVMin::NoRegister)
    Reg = MatchRegisterAltName(Name);
  return Reg;
}

ParseStatus RVMinAsmParser::tryParseRegister(MCRegister &Reg, SMLoc &StartLoc,
                                             SMLoc &EndLoc) {
  if (getLexer().isNot(AsmToken::Identifier))
    return ParseStatus::NoMatch;

  const AsmToken &Tok = getLexer().getTok();
  Reg = matchRegister(Tok.getIdentifier());
  if (Reg == RVMin::NoRegister)
    return ParseStatus::NoMatch;

  StartLoc = Tok.getLoc();
  EndLoc = Tok.getEndLoc();
  Lex();
  return ParseStatus::Success;
}

bool RVMinAsmParser::parseRegister(MCRegister &Reg, SMLoc &StartLoc,
                                   SMLoc &EndLoc) {
  ParseStatus Res = tryParseRegister(Reg, StartLoc, EndLoc);
  if (Res.isFailure())
    return Error(StartLoc, "invalid register name");
  if (Res.isSuccess())
    return false;
  return true;
}

ParseStatus RVMinAsmParser::parseRegister(OperandVector &Operands) {
  MCRegister Reg;
  SMLoc StartLoc, EndLoc;
  if (tryParseRegister(Reg, StartLoc, EndLoc).isNoMatch())
    return ParseStatus::NoMatch;
  Operands.push_back(RVMinOperand::createReg(Reg, StartLoc, EndLoc));
  return ParseStatus::Success;
}

ParseStatus RVMinAsmParser::parseExpression(OperandVector &Operands) {
  SMLoc StartLoc = getLexer().getTok().getLoc();
  const MCExpr *Expr;
  if (getParser().parseExpression(Expr))
    return ParseStatus::Failure;

  SMLoc EndLoc = getLexer().getTok().getLoc();
  Operands.push_back(RVMinOperand::createImm(Expr, StartLoc, EndLoc));
  return ParseStatus::Success;
}

ParseStatus RVMinAsmParser::parseSpecifierOperand(OperandVector &Operands) {
  SMLoc StartLoc = getLoc();
  Lex(); // '%'

  if (getLexer().isNot(AsmToken::Identifier))
    return Error(getLoc(), "expected relocation specifier after '%'");

  StringRef Name = getLexer().getTok().getIdentifier();
  std::optional<rvmin::Specifier> Spec = rvmin::parseSpecifierName(Name);
  if (!Spec)
    return Error(getLoc(), "invalid relocation specifier");
  Lex();

  if (parseToken(AsmToken::LParen, "expected '('"))
    return ParseStatus::Failure;

  const MCExpr *SubExpr;
  SMLoc EndLoc;
  if (getParser().parseParenExpression(SubExpr, EndLoc))
    return ParseStatus::Failure;

  Operands.push_back(RVMinOperand::createImm(
      MCSpecifierExpr::create(SubExpr, *Spec, getContext(), StartLoc),
      StartLoc, EndLoc));

  // `%lo(sym)(a0)`: a symbolic displacement with a base register.
  if (getLexer().is(AsmToken::LParen)) {
    if (parseMemOpBaseReg(Operands).isFailure())
      return Error(getLoc(), "expected memory base register");
  }
  return ParseStatus::Success;
}

ParseStatus RVMinAsmParser::parseMemOpBaseReg(OperandVector &Operands) {
  // The matcher compares these literal tokens against the '(' and ')' in the
  // instruction's AsmString ($imm12($rs1)), so they must be part of the
  // operand vector.
  if (parseToken(AsmToken::LParen, "expected '('"))
    return ParseStatus::Failure;
  Operands.push_back(RVMinOperand::createToken("(", getLoc()));

  if (parseRegister(Operands).isFailure())
    return Error(getLoc(), "expected register");

  if (parseToken(AsmToken::RParen, "expected ')'"))
    return ParseStatus::Failure;
  Operands.push_back(RVMinOperand::createToken(")", getLoc()));

  return ParseStatus::Success;
}

ParseStatus RVMinAsmParser::parseLiteralValues(unsigned Size) {
  auto parseOne = [&]() -> bool {
    const MCExpr *Value;
    if (getParser().parseExpression(Value))
      return true;
    getParser().getStreamer().emitValue(Value, Size, getLoc());
    return false;
  };
  return parseMany(parseOne) ? ParseStatus::Failure : ParseStatus::Success;
}

ParseStatus RVMinAsmParser::parseDirective(AsmToken DirectiveID) {
  // The generic parser knows .byte/.4byte/.8byte; provide the GNU spellings
  // used by hand-written RISC-V assembly.
  StringRef IDVal = DirectiveID.getIdentifier();
  if (IDVal == ".word" || IDVal == ".long")
    return parseLiteralValues(4);
  if (IDVal == ".half" || IDVal == ".short")
    return parseLiteralValues(2);
  return ParseStatus::NoMatch;
}

bool RVMinAsmParser::parseOperand(OperandVector &Operands, StringRef Mnemonic) {
  if (parseRegister(Operands).isSuccess())
    return false;

  // `fcvt.w.s` and `fcvt.wu.s` print an explicit rounding mode, e.g.
  // `fcvt.w.s a0, fa1, rtz`.  A bare identifier would otherwise be parsed as
  // a symbol expression, so hand the matcher a literal token operand instead;
  // it compares it against the `rtz` text in the instruction's AsmString.
  if (Mnemonic.starts_with("fcvt.") &&
      getLexer().is(AsmToken::Identifier) &&
      getLexer().getTok().getIdentifier() == "rtz") {
    Operands.push_back(
        RVMinOperand::createToken("rtz", getLexer().getTok().getLoc()));
    Lex();
    return false;
  }

  // %hi(sym) / %lo(sym): a relocation specifier around an expression.
  if (getLexer().is(AsmToken::Percent)) {
    if (parseSpecifierOperand(Operands).isFailure())
      return true;
    return false;
  }

  // `(rs1)` is a memory operand with a zero displacement, e.g.
  // `lr.w a0, (a1)`.  Expressions can also start with a parenthesis, so only
  // take this path when the token after `(` is a register.
  if (getLexer().is(AsmToken::LParen)) {
    const AsmToken &Next = getLexer().peekTok();
    if (Next.is(AsmToken::Identifier) &&
        matchRegister(Next.getIdentifier()) != RVMin::NoRegister) {
      if (parseMemOpBaseReg(Operands).isFailure())
        return Error(getLoc(), "expected memory base register");
      return false;
    }
  }

  if (parseExpression(Operands).isSuccess()) {
    // `8(sp)` is two operands: the offset expression and the base register.
    if (getLexer().is(AsmToken::LParen)) {
      if (parseMemOpBaseReg(Operands).isFailure())
        return Error(getLexer().getLoc(), "expected memory base register");
    }
    return false;
  }

  return Error(getLexer().getLoc(), "unknown operand");
}

bool RVMinAsmParser::parseInstruction(ParseInstructionInfo &Info,
                                      StringRef Name, SMLoc NameLoc,
                                      OperandVector &Operands) {
  Operands.push_back(RVMinOperand::createToken(Name, NameLoc));

  if (getLexer().is(AsmToken::EndOfStatement)) {
    Lex(); // Consume the end of statement.
    return false;
  }

  if (parseOperand(Operands, Name))
    return true;

  while (parseOptionalToken(AsmToken::Comma)) {
    if (parseOperand(Operands, Name))
      return true;
  }

  if (getParser().parseEOL("unexpected token")) {
    getParser().eatToEndOfStatement();
    return true;
  }
  return false;
}

bool RVMinAsmParser::matchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                                             OperandVector &Operands,
                                             MCStreamer &Out,
                                             uint64_t &ErrorInfo,
                                             bool MatchingInlineAsm) {
  MCInst Inst;
  // The five-argument overload additionally reports *which* subtarget
  // features were missing, so `mul` without `-mattr=+m` can be diagnosed
  // instead of silently doing nothing.
  FeatureBitset MissingFeatures;
  unsigned MatchResult = MatchInstructionImpl(
      Operands, Inst, ErrorInfo, MissingFeatures, MatchingInlineAsm);

  switch (MatchResult) {
  case Match_Success:
    Inst.setLoc(IDLoc);
    Out.emitInstruction(Inst, *STI);
    return false;
  case Match_MissingFeature: {
    // `getSubtargetFeatureName` is generated from the AssemblerPredicate in
    // RVMin.td, e.g. "'M' (Integer Multiplication and Division)".
    bool First = true;
    std::string Msg = "instruction requires the following:";
    for (unsigned Feature : MissingFeatures) {
      Msg += First ? " " : ", ";
      Msg += getSubtargetFeatureName(Feature);
      First = false;
    }
    return Error(IDLoc, Msg);
  }
  case Match_MnemonicFail:
    return Error(IDLoc, "invalid instruction mnemonic");
  case Match_InvalidOperand: {
    SMLoc Loc = IDLoc;
    if (ErrorInfo != ~0U) {
      if (ErrorInfo >= Operands.size())
        return Error(Loc, "too few operands for instruction");
      Loc = Operands[ErrorInfo]->getStartLoc();
      if (Loc == SMLoc())
        Loc = IDLoc;
    }
    return Error(Loc, "invalid operand for instruction");
  }
  default:
    return true;
  }
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeRVMinAsmParser() {
  RegisterMCAsmParser<RVMinAsmParser> X(getTheRVMinTarget());
}

#define GET_REGISTER_MATCHER
#define GET_SUBTARGET_FEATURE_NAME
#define GET_MATCHER_IMPLEMENTATION
#include "RVMinGenAsmMatcher.inc"
