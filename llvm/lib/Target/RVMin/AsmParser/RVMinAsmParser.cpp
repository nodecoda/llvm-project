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
  ParseStatus parseCSRSystemRegister(OperandVector &Operands);
  ParseStatus parseVType(OperandVector &Operands);
  ParseStatus parseV0MaskReg(OperandVector &Operands);
  ParseStatus parseMemOpBaseReg(OperandVector &Operands);

  /// @name Auto-generated Matcher Functions
  /// {

#define GET_ASSEMBLER_HEADER
#include "RVMinGenAsmMatcher.inc"

  /// }

public:
  // Chapter 46: the generated matcher reports a near-miss for a malformed CSR
  // operand through this enum.  The block must live inside the parser class.
  enum RVMinMatchResultTy : unsigned {
    Match_Dummy = FIRST_TARGET_MATCH_RESULT_TY,
#define GET_OPERAND_DIAGNOSTIC_TYPES
#include "RVMinGenAsmMatcher.inc"
#undef GET_OPERAND_DIAGNOSTIC_TYPES
  };

private:

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
  enum KindTy { Token, Register, Immediate, SystemReg } Kind;

  SMLoc StartLoc, EndLoc;
  union {
    StringRef Tok;
    MCRegister Reg;
    const MCExpr *Imm;
    unsigned SysReg;
  };
  /// Chapter 52: true when the operand came from parseV0MaskReg, i.e. the
  /// source wrote the `.t` suffix.  The masked ALU forms require it;
  /// vmerge's explicit-mask operand does not.
  bool IsMaskToken = false;

public:
  RVMinOperand(KindTy Kind) : MCParsedAsmOperand(), Kind(Kind) {}

  bool isToken() const override { return Kind == Token; }
  bool isReg() const override { return Kind == Register; }
  bool isImm() const override { return Kind == Immediate; }
  bool isMem() const override { return false; }

  unsigned getSysReg() const {
    assert(Kind == SystemReg && "not a system register");
    return SysReg;
  }

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
    case SystemReg:
      O << "SystemReg " << SysReg;
      break;
    }
  }

  bool evaluateConstantImm(int64_t &Value) const {
    return Kind == Immediate && Imm->evaluateAsAbsolute(Value);
  }

  // Chapter 46: a CSR operand parsed by parseCSRSystemRegister below.  The
  // generated matcher validates the class with this predicate.
  bool isCSRSystemRegister() const { return Kind == SystemReg; }

  // Chapter 52: the generated matcher validates the mask operand class.
  bool isV0Reg() const { return Kind == Register && Reg == RVMin::V0; }
  bool isV0TReg() const { return IsMaskToken && isV0Reg(); }

  // Chapter 47: vsetivli encodes 10 vtype bits and vsetvli 11.  parseVType
  // folds both the token spelling and a raw number into an immediate; these
  // predicates let the generated matcher reject an out-of-range number.
  bool isVTypeI10() const {
    int64_t Value;
    return evaluateConstantImm(Value) && isUInt<10>(Value);
  }
  bool isVTypeI11() const {
    int64_t Value;
    return evaluateConstantImm(Value) && isUInt<11>(Value);
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
  // Chapter 47: vadd.vi and vmv.v.i carry a signed 5-bit immediate.
  bool isSImm5() const {
    int64_t Value;
    return evaluateConstantImm(Value) && isInt<5>(Value);
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

  void addCSRSystemRegisterOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "invalid number of operands");
    Inst.addOperand(MCOperand::createImm(SysReg));
  }

  static std::unique_ptr<RVMinOperand> createToken(StringRef Tok, SMLoc Loc) {
    auto Op = std::make_unique<RVMinOperand>(Token);
    Op->Tok = Tok;
    Op->StartLoc = Loc;
    Op->EndLoc = Loc;
    return Op;
  }
  static std::unique_ptr<RVMinOperand> createMaskReg(MCRegister Reg, SMLoc S,
                                                     SMLoc E) {
    auto Op = std::make_unique<RVMinOperand>(Register);
    Op->Reg = Reg;
    Op->StartLoc = S;
    Op->EndLoc = E;
    Op->IsMaskToken = true;
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
  static std::unique_ptr<RVMinOperand> createSysReg(unsigned Encoding,
                                                   SMLoc Start, SMLoc End) {
    auto Op = std::make_unique<RVMinOperand>(SystemReg);
    Op->SysReg = Encoding;
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

// Chapter 52: `v0.t` is one identifier token.  Strip the suffix, resolve the
// register, and leave the predicate (isV0Reg) to reject anything but v0.
ParseStatus RVMinAsmParser::parseV0MaskReg(OperandVector &Operands) {
  if (getLexer().isNot(AsmToken::Identifier))
    return ParseStatus::NoMatch;

  const AsmToken &Tok = getLexer().getTok();
  StringRef Name = Tok.getIdentifier();
  if (!Name.consume_back(".t"))
    return ParseStatus::NoMatch;

  MCRegister Reg = matchRegister(Name);
  if (Reg == RVMin::NoRegister)
    return ParseStatus::Failure;

  Operands.push_back(
      RVMinOperand::createMaskReg(Reg, Tok.getLoc(), Tok.getEndLoc()));
  Lex();
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

// Chapter 46: a CSR operand is either a name from the rvmin:: table or an
// expression that folds to a 12-bit number.  Keeping both spellings in one
// parser means `csrr a0, mstatus` and `csrr a0, 0x300` produce identical
// MCInsts, and the disassembler can print whichever the table knows.
ParseStatus RVMinAsmParser::parseCSRSystemRegister(OperandVector &Operands) {
  SMLoc S = getLoc();

  if (getLexer().is(AsmToken::Identifier)) {
    StringRef Name;
    if (getParser().parseIdentifier(Name))
      return ParseStatus::Failure;
    unsigned Encoding;
    if (!rvmin::parseCSRName(Name, Encoding))
      return Error(S, "unknown CSR name '" + Name + "'");
    Operands.push_back(RVMinOperand::createSysReg(Encoding, S, getLoc()));
    return ParseStatus::Success;
  }

  const MCExpr *Expr;
  if (getParser().parseExpression(Expr))
    return ParseStatus::Failure;

  int64_t Value;
  if (!Expr->evaluateAsAbsolute(Value) || !isUInt<12>(Value))
    return Error(S, "CSR number must be an integer in the range 0..4095");

  Operands.push_back(
      RVMinOperand::createSysReg(static_cast<unsigned>(Value), S, getLoc()));
  return ParseStatus::Success;
}

// Chapter 47: the vtype operand.  Assembly spells it as four tokens
// (`e32, m1, ta, ma`) or as the raw integer, and both forms must produce the
// same 8-bit vtype value:
//
//   vma << 7 | vta << 6 | vsew << 3 | vlmul
//
// The parser consumes the commas inside the token group so the generated
// matcher sees one operand, not four.
ParseStatus RVMinAsmParser::parseVType(OperandVector &Operands) {
  SMLoc S = getLoc();

  auto parseComma = [&]() -> bool {
    if (!getParser().parseOptionalToken(AsmToken::Comma)) {
      Error(getLoc(), "expected ',' in vtype");
      return false;
    }
    return true;
  };

  if (getLexer().is(AsmToken::Identifier)) {
    StringRef Name;
    if (getParser().parseIdentifier(Name))
      return ParseStatus::Failure;

    int SEW = -1;
    if (Name == "e8")
      SEW = 0;
    else if (Name == "e16")
      SEW = 1;
    else if (Name == "e32")
      SEW = 2;
    else if (Name == "e64")
      SEW = 3;
    if (SEW < 0)
      return Error(S, "unknown vtype SEW '" + Name + "'");
    if (!parseComma())
      return ParseStatus::Failure;

    if (getLexer().isNot(AsmToken::Identifier))
      return Error(getLoc(), "expected an LMUL token after the SEW");
    if (getParser().parseIdentifier(Name))
      return ParseStatus::Failure;

    int LMUL = -1;
    if (Name == "mf8")
      LMUL = 5;
    else if (Name == "mf4")
      LMUL = 6;
    else if (Name == "mf2")
      LMUL = 7;
    else if (Name == "m1")
      LMUL = 0;
    else if (Name == "m2")
      LMUL = 1;
    else if (Name == "m4")
      LMUL = 2;
    else if (Name == "m8")
      LMUL = 3;
    if (LMUL < 0)
      return Error(S, "unknown vtype LMUL '" + Name + "'");
    if (!parseComma())
      return ParseStatus::Failure;

    if (getLexer().isNot(AsmToken::Identifier))
      return Error(getLoc(), "expected a tail-policy token (tu/ta)");
    if (getParser().parseIdentifier(Name))
      return ParseStatus::Failure;
    int VTA;
    if (Name == "tu")
      VTA = 0;
    else if (Name == "ta")
      VTA = 1;
    else
      return Error(S, "unknown vtype tail policy '" + Name + "'");
    if (!parseComma())
      return ParseStatus::Failure;

    if (getLexer().isNot(AsmToken::Identifier))
      return Error(getLoc(), "expected a mask-policy token (mu/ma)");
    if (getParser().parseIdentifier(Name))
      return ParseStatus::Failure;
    int VMA;
    if (Name == "mu")
      VMA = 0;
    else if (Name == "ma")
      VMA = 1;
    else
      return Error(S, "unknown vtype mask policy '" + Name + "'");

    unsigned Encoding = (VMA << 7) | (VTA << 6) | (SEW << 3) | LMUL;
    Operands.push_back(RVMinOperand::createImm(
        MCConstantExpr::create(Encoding, getParser().getContext()), S,
        getLoc()));
    return ParseStatus::Success;
  }

  const MCExpr *Expr;
  if (getParser().parseExpression(Expr))
    return ParseStatus::Failure;

  int64_t Value;
  if (!Expr->evaluateAsAbsolute(Value) || !isUInt<11>(Value))
    return Error(S, "vtype must be an integer in the range 0..2047");

  Operands.push_back(RVMinOperand::createImm(Expr, S, getLoc()));
  return ParseStatus::Success;
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
  // Chapter 46: operands whose grammar is target-specific (currently only the
  // CSR name-or-number operand) get the first chance.  The generated matcher
  // knows from the mnemonic which operand classes have a custom parser, so
  // this is a no-op for every instruction that has none.
  ParseStatus Custom =
      MatchOperandParserImpl(Operands, Mnemonic, /*ParseForAllFeatures=*/true);
  if (Custom.isSuccess())
    return false;
  if (Custom.isFailure())
    return true;

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
    // Chapter 52: the mask lives in v0, so a masked instruction must not
    // write it.  vmerge always consumes v0; the masked ALU forms put the
    // mask operand last (operand 3).  Sources may be v0, the destination
    // may not, matching the upstream assembler.
    if (Inst.getNumOperands() == 4 && Inst.getOperand(3).isReg() &&
        Inst.getOperand(3).getReg() == RVMin::V0 &&
        Inst.getOperand(0).isReg() && Inst.getOperand(0).getReg() == RVMin::V0) {
      if (Inst.getOpcode() == RVMin::VMERGE_VVM)
        return Error(IDLoc, "the destination vector register group cannot be V0");
      return Error(IDLoc, "the destination vector register group cannot "
                          "overlap the mask register");
    }
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
  case Match_InvalidCSRSystemRegister: {
    SMLoc Loc = IDLoc;
    if (ErrorInfo != ~0U && ErrorInfo < Operands.size())
      Loc = Operands[ErrorInfo]->getStartLoc();
    return Error(Loc, "expected a CSR name or a 12-bit CSR number");
  }
  case Match_InvalidVType: {
    SMLoc Loc = IDLoc;
    if (ErrorInfo != ~0U && ErrorInfo < Operands.size())
      Loc = Operands[ErrorInfo]->getStartLoc();
    return Error(Loc, "expected a vtype (e32, m1, ta, ma) or an integer");
  }
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
  case Match_InvalidVMaskRegister:
  case Match_InvalidVMergeMaskRegister: {
    SMLoc Loc = IDLoc;
    if (ErrorInfo != ~0U && ErrorInfo < Operands.size())
      Loc = Operands[ErrorInfo]->getStartLoc();
    return Error(Loc, MatchResult == Match_InvalidVMaskRegister
                          ? "operand must be v0.t"
                          : "operand must be v0");
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
