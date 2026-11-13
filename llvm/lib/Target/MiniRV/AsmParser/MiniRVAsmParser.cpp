//===-- MiniRVAsmParser.cpp - Parse MiniRV assembly to MCInst ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The parser is deliberately small: the mnemonic table, the operand classes and
// the matcher all come from MiniRVGenAsmMatcher.inc, which TableGen produces
// from the records auto-td writes into AutoTdInstrInfo.inc.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/MiniRVMCTargetDesc.h"
#include "TargetInfo/MiniRVTargetInfo.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCParser/AsmLexer.h"
#include "llvm/MC/MCParser/MCParsedAsmOperand.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

namespace {

class MiniRVAsmParser : public MCTargetAsmParser {
  MCAsmParser &Parser;

  bool matchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                               OperandVector &Operands, MCStreamer &Out,
                               uint64_t &ErrorInfo,
                               bool MatchingInlineAsm) override;

  bool parseRegister(MCRegister &Reg, SMLoc &StartLoc, SMLoc &EndLoc) override;
  ParseStatus tryParseRegister(MCRegister &Reg, SMLoc &StartLoc,
                               SMLoc &EndLoc) override;

  bool parseInstruction(ParseInstructionInfo &Info, StringRef Name,
                        SMLoc NameLoc, OperandVector &Operands) override;

  bool ParseOperand(OperandVector &Operands);

  bool ParseMemOperand(SMLoc StartLoc, const MCExpr *Offset,
                       OperandVector &Operands);

  MCAsmParser &getParser() const { return Parser; }
  AsmLexer &getLexer() const { return Parser.getLexer(); }

public:
  enum MiniRVMatchResultTy : unsigned {
    Match_Dummy = FIRST_TARGET_MATCH_RESULT_TY,
#define GET_OPERAND_DIAGNOSTIC_TYPES
#include "MiniRVGenAsmMatcher.inc"
#undef GET_OPERAND_DIAGNOSTIC_TYPES
  };

private:
  /// @name Auto-generated Matcher Functions
  /// {

#define GET_ASSEMBLER_HEADER
#include "MiniRVGenAsmMatcher.inc"

  /// }

public:
  MiniRVAsmParser(const MCSubtargetInfo &STI, MCAsmParser &Parser,
                  const MCInstrInfo &MII)
      : MCTargetAsmParser(STI, MII), Parser(Parser) {
    MCAsmParserExtension::Initialize(Parser);
    // GNU RISC-V assembly spells the data directives .half/.word/.dword; the
    // generic parser only knows the numeric-size names.
    Parser.addAliasForDirective(".half", ".2byte");
    Parser.addAliasForDirective(".word", ".4byte");
    Parser.addAliasForDirective(".dword", ".8byte");
    setAvailableFeatures(ComputeAvailableFeatures(STI.getFeatureBits()));
  }
};

/// A parsed MiniRV assembly operand: a mnemonic token, a register or an
/// immediate expression.
class MiniRVOperand : public MCParsedAsmOperand {
  enum KindTy { k_Tok, k_Reg, k_Imm } Kind;

  union {
    const MCExpr *Imm;
    MCRegister Reg;
    StringRef Tok;
  };

  SMLoc Start, End;

public:
  MiniRVOperand(StringRef Tok, SMLoc S)
      : Kind(k_Tok), Tok(Tok), Start(S), End(S) {}
  MiniRVOperand(KindTy Kind, MCRegister Reg, SMLoc S, SMLoc E)
      : Kind(Kind), Reg(Reg), Start(S), End(E) {}
  MiniRVOperand(const MCExpr *Imm, SMLoc S, SMLoc E)
      : Kind(k_Imm), Imm(Imm), Start(S), End(E) {}

  void addRegOperands(MCInst &Inst, unsigned N) const {
    assert(Kind == k_Reg && "Unexpected operand kind");
    assert(N == 1 && "Invalid number of operands!");
    Inst.addOperand(MCOperand::createReg(Reg));
  }

  void addImmOperands(MCInst &Inst, unsigned N) const {
    assert(Kind == k_Imm && "Unexpected operand kind");
    assert(N == 1 && "Invalid number of operands!");
    if (const auto *CE = dyn_cast<MCConstantExpr>(Imm))
      Inst.addOperand(MCOperand::createImm(CE->getValue()));
    else
      Inst.addOperand(MCOperand::createExpr(Imm));
  }

  bool isReg() const override { return Kind == k_Reg; }
  bool isImm() const override { return Kind == k_Imm; }
  bool isToken() const override { return Kind == k_Tok; }
  bool isMem() const override { return false; }

  bool isSImm12() const {
    if (Kind != k_Imm)
      return false;
    int64_t Val;
    return Imm->evaluateAsAbsolute(Val) && isInt<12>(Val);
  }

  bool isUImm20() const {
    if (Kind != k_Imm)
      return false;
    int64_t Val;
    return Imm->evaluateAsAbsolute(Val) && isUInt<20>(Val);
  }

  bool isUImm6() const {
    if (Kind != k_Imm)
      return false;
    int64_t Val;
    return Imm->evaluateAsAbsolute(Val) && isUInt<6>(Val);
  }

  bool isUImm5() const {
    if (Kind != k_Imm)
      return false;
    int64_t Val;
    return Imm->evaluateAsAbsolute(Val) && isUInt<5>(Val);
  }

  /// A branch target is either an absolute offset with an implicit zero low
  /// bit, or a label whose value the assembler does not know yet.
  bool isBranchTarget() const {
    if (Kind != k_Imm)
      return false;
    int64_t Val;
    if (Imm->evaluateAsAbsolute(Val))
      return isShiftedInt<12, 1>(Val);
    return true;
  }

  bool isJalTarget() const {
    if (Kind != k_Imm)
      return false;
    int64_t Val;
    if (Imm->evaluateAsAbsolute(Val))
      return isShiftedInt<20, 1>(Val);
    return true;
  }

  StringRef getToken() const {
    assert(Kind == k_Tok && "Invalid access!");
    return Tok;
  }

  MCRegister getReg() const override {
    assert(Kind == k_Reg && "Invalid access!");
    return Reg;
  }

  static std::unique_ptr<MiniRVOperand> CreateToken(StringRef Str, SMLoc S) {
    return std::make_unique<MiniRVOperand>(Str, S);
  }

  static std::unique_ptr<MiniRVOperand> CreateReg(MCRegister Reg, SMLoc S,
                                                  SMLoc E) {
    return std::make_unique<MiniRVOperand>(k_Reg, Reg, S, E);
  }

  static std::unique_ptr<MiniRVOperand> CreateImm(const MCExpr *Val, SMLoc S,
                                                  SMLoc E) {
    return std::make_unique<MiniRVOperand>(Val, S, E);
  }

  SMLoc getStartLoc() const override { return Start; }
  SMLoc getEndLoc() const override { return End; }

  void print(raw_ostream &O, const MCAsmInfo &MAI) const override {
    switch (Kind) {
    case k_Tok:
      O << "Token " << Tok;
      break;
    case k_Reg:
      O << "Register " << Reg.id();
      break;
    case k_Imm:
      O << "Immediate ";
      MAI.printExpr(O, *Imm);
      break;
    }
  }
};

} // end anonymous namespace

bool MiniRVAsmParser::matchAndEmitInstruction(SMLoc Loc, unsigned &Opcode,
                                              OperandVector &Operands,
                                              MCStreamer &Out,
                                              uint64_t &ErrorInfo,
                                              bool MatchingInlineAsm) {
  MCInst Inst;
  unsigned MatchResult =
      MatchInstructionImpl(Operands, Inst, ErrorInfo, MatchingInlineAsm);

  switch (MatchResult) {
  case Match_Success:
    Inst.setLoc(Loc);
    Out.emitInstruction(Inst, *STI);
    return false;
  case Match_MnemonicFail:
    return Error(Loc, "invalid instruction mnemonic");
  case Match_InvalidSImm12:
  case Match_InvalidUImm20:
  case Match_InvalidUImm6:
  case Match_InvalidUImm5:
  case Match_InvalidBranchTarget:
  case Match_InvalidJalTarget:
  case Match_InvalidOperand: {
    SMLoc ErrorLoc = Loc;
    if (ErrorInfo != ~0U) {
      if (ErrorInfo >= Operands.size())
        return Error(ErrorLoc, "too few operands for instruction");
      ErrorLoc = Operands[ErrorInfo]->getStartLoc();
      if (ErrorLoc == SMLoc())
        ErrorLoc = Loc;
    }
    if (MatchResult == Match_InvalidSImm12)
      return Error(ErrorLoc, "operand must be a 12-bit signed immediate");
    if (MatchResult == Match_InvalidUImm20)
      return Error(ErrorLoc, "operand must be a 20-bit unsigned immediate");
    if (MatchResult == Match_InvalidUImm6)
      return Error(ErrorLoc, "operand must be a 6-bit unsigned immediate");
    if (MatchResult == Match_InvalidUImm5)
      return Error(ErrorLoc, "operand must be a 5-bit unsigned immediate");
    if (MatchResult == Match_InvalidBranchTarget)
      return Error(ErrorLoc, "operand must be a PC-relative branch target");
    if (MatchResult == Match_InvalidJalTarget)
      return Error(ErrorLoc, "operand must be a PC-relative jump target");
    return Error(ErrorLoc, "invalid operand for instruction");
  }
  default:
    return Error(Loc, "invalid instruction");
  }
}

// Auto-generated by TableGen.
static MCRegister MatchRegisterName(StringRef Name);

bool MiniRVAsmParser::parseRegister(MCRegister &Reg, SMLoc &StartLoc,
                                    SMLoc &EndLoc) {
  ParseStatus Res = tryParseRegister(Reg, StartLoc, EndLoc);
  if (Res.isFailure())
    return Error(StartLoc, "invalid register name");
  if (Res.isSuccess())
    return false;
  if (Res.isNoMatch())
    return true;

  llvm_unreachable("unknown parse status");
}

ParseStatus MiniRVAsmParser::tryParseRegister(MCRegister &Reg, SMLoc &StartLoc,
                                              SMLoc &EndLoc) {
  if (getLexer().getKind() == AsmToken::Identifier) {
    Reg = MatchRegisterName(getLexer().getTok().getIdentifier().lower());
    if (Reg == MiniRV::NoRegister)
      return ParseStatus::NoMatch;

    const AsmToken &T = getParser().getTok();
    StartLoc = T.getLoc();
    EndLoc = T.getEndLoc();
    getLexer().Lex(); // Eat the register token.
    return ParseStatus::Success;
  }
  return ParseStatus::Failure;
}

/// Parse an optional ``(reg)`` suffix after a memory offset.  The offset and
/// the base register are handed to the matcher as two operands, in the order
/// the assembly string mentions them, which is what the generated matcher
/// expects.
bool MiniRVAsmParser::ParseMemOperand(SMLoc StartLoc, const MCExpr *Offset,
                                      OperandVector &Operands) {
  if (getLexer().isNot(AsmToken::LParen))
    return true;
  SMLoc LParenLoc = getLexer().getLoc();
  getLexer().Lex(); // Eat '('.

  MCRegister RegNo;
  SMLoc RegStart, RegEnd;
  if (parseRegister(RegNo, RegStart, RegEnd))
    return true;

  if (getLexer().isNot(AsmToken::RParen))
    return Error(getLexer().getLoc(), "expected ')' in memory operand");
  SMLoc RParenLoc = getLexer().getLoc();
  getLexer().Lex(); // Eat ')'.

  // The parentheses are operands too: the generated matcher treats them as
  // literal tokens, which is how it knows this is a memory reference.
  Operands.push_back(MiniRVOperand::CreateImm(Offset, StartLoc, LParenLoc));
  Operands.push_back(MiniRVOperand::CreateToken("(", LParenLoc));
  Operands.push_back(MiniRVOperand::CreateReg(RegNo, RegStart, RegEnd));
  Operands.push_back(MiniRVOperand::CreateToken(")", RParenLoc));
  return false;
}

bool MiniRVAsmParser::ParseOperand(OperandVector &Operands) {
  switch (getLexer().getKind()) {
  default:
    return true;
  case AsmToken::Identifier: {
    MCRegister RegNo;
    SMLoc StartLoc, EndLoc;
    if (!parseRegister(RegNo, StartLoc, EndLoc)) {
      Operands.push_back(MiniRVOperand::CreateReg(RegNo, StartLoc, EndLoc));
      return false;
    }

    // Not a register, so it is a label used as a branch or jump target.
    SMLoc SymLoc = getParser().getTok().getLoc();
    const MCExpr *Val;
    if (getParser().parseExpression(Val))
      return true;
    Operands.push_back(
        MiniRVOperand::CreateImm(Val, SymLoc, getParser().getTok().getLoc()));
    return false;
  }
  case AsmToken::LParen: {
    // "ld x1, (x2)": a memory operand with a zero offset.
    SMLoc StartLoc = getLexer().getLoc();
    const MCExpr *Zero = MCConstantExpr::create(0, getContext());
    return ParseMemOperand(StartLoc, Zero, Operands);
  }
  case AsmToken::Integer:
  case AsmToken::Plus:
  case AsmToken::Minus: {
    SMLoc StartLoc = getParser().getTok().getLoc();
    const MCExpr *Val;
    if (getParser().parseExpression(Val))
      return true;

    if (getLexer().is(AsmToken::LParen))
      return ParseMemOperand(StartLoc, Val, Operands);

    Operands.push_back(
        MiniRVOperand::CreateImm(Val, StartLoc, getParser().getTok().getLoc()));
    return false;
  }
  }
}

bool MiniRVAsmParser::parseInstruction(ParseInstructionInfo &Info,
                                       StringRef Name, SMLoc NameLoc,
                                       OperandVector &Operands) {
  Operands.push_back(MiniRVOperand::CreateToken(Name, NameLoc));

  if (getLexer().is(AsmToken::EndOfStatement)) {
    getLexer().Lex(); // Consume the EndOfStatement.
    return false;
  }

  if (ParseOperand(Operands))
    return true;

  while (getLexer().is(AsmToken::Comma)) {
    getLexer().Lex(); // Eat the comma.
    if (ParseOperand(Operands))
      return true;
  }

  if (!getLexer().is(AsmToken::EndOfStatement))
    return Error(getLexer().getLoc(), "unexpected token in operand");

  getLexer().Lex();
  return false;
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeMiniRVAsmParser() {
  RegisterMCAsmParser<MiniRVAsmParser> X(getTheMiniRVTarget());
}

#define GET_REGISTER_MATCHER
#define GET_MATCHER_IMPLEMENTATION
#include "MiniRVGenAsmMatcher.inc"
