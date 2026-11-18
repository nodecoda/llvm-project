//===-- GPUMinAsmParser.cpp - Parse GPUMin assembly to MCInst -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A small hand-written parser for GM32 assembly.
//
// The parser only recognizes three operand shapes:
//
//   register      : r0, r31, zero, sp, lr
//   expression    : 42, -4, 0x10, label, label+4
//   memory        : expression '(' register ')'   e.g. 8(r2)
//
// Everything else (mnemonic -> instruction, operand-range checks) is handled by
// the TableGen-generated matcher.  Chapter 00 has no real instructions yet, so
// the matcher rejects every mnemonic; the scaffolding is here so that chapter
// 02 can add the ISA without restructuring the parser.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/GPUMinMCTargetDesc.h"
#include "TargetInfo/GPUMinTargetInfo.h"
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

namespace {

class GPUMinAsmParser : public MCTargetAsmParser {
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
  bool parseOperand(OperandVector &Operands, StringRef Mnemonic);
  ParseStatus parseMemOpBaseReg(OperandVector &Operands);

  /// @name Auto-generated Matcher Functions
  /// {

#define GET_ASSEMBLER_HEADER
#include "GPUMinGenAsmMatcher.inc"

  /// }

public:
  GPUMinAsmParser(const MCSubtargetInfo &STI, MCAsmParser &Parser,
                  const MCInstrInfo &MII)
      : MCTargetAsmParser(STI, MII), Parser(Parser) {
    MCAsmParserExtension::Initialize(Parser);
    setAvailableFeatures(ComputeAvailableFeatures(STI.getFeatureBits()));
  }
};

/// A parsed GM32 assembly operand.
class GPUMinOperand : public MCParsedAsmOperand {
  enum KindTy { Token, Register, Immediate } Kind;

  SMLoc StartLoc, EndLoc;
  union {
    StringRef Tok;
    MCRegister Reg;
    const MCExpr *Imm;
  };

public:
  GPUMinOperand(KindTy Kind) : MCParsedAsmOperand(), Kind(Kind) {}

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

  void print(raw_ostream &OS, const MCAsmInfo &MAI) const override {}

  /// Matcher predicates for the register and immediate operand classes.
  bool isGPR() const { return Kind == Register; }

  void addRegOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of operands!");
    Inst.addOperand(MCOperand::createReg(getReg()));
  }

  void addImmOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of operands!");
    const MCExpr *Expr = getImm();
    if (auto *CE = dyn_cast<MCConstantExpr>(Expr)) {
      Inst.addOperand(MCOperand::createImm(CE->getValue()));
      return;
    }
    Inst.addOperand(MCOperand::createExpr(Expr));
  }

  static std::unique_ptr<GPUMinOperand> createToken(StringRef Str, SMLoc S) {
    auto Op = std::make_unique<GPUMinOperand>(Token);
    Op->Tok = Str;
    Op->StartLoc = S;
    Op->EndLoc = S;
    return Op;
  }

  static std::unique_ptr<GPUMinOperand> createReg(MCRegister Reg, SMLoc S,
                                                  SMLoc E) {
    auto Op = std::make_unique<GPUMinOperand>(Register);
    Op->Reg = Reg;
    Op->StartLoc = S;
    Op->EndLoc = E;
    return Op;
  }

  static std::unique_ptr<GPUMinOperand> createImm(const MCExpr *Expr, SMLoc S,
                                                  SMLoc E) {
    auto Op = std::make_unique<GPUMinOperand>(Immediate);
    Op->Imm = Expr;
    Op->StartLoc = S;
    Op->EndLoc = E;
    return Op;
  }
};

} // end anonymous namespace

// Auto-generated by TableGen.
static MCRegister MatchRegisterName(StringRef Name);

ParseStatus GPUMinAsmParser::tryParseRegister(MCRegister &Reg, SMLoc &StartLoc,
                                              SMLoc &EndLoc) {
  if (getLexer().isNot(AsmToken::Identifier))
    return ParseStatus::NoMatch;

  const AsmToken &Tok = getLexer().getTok();
  Reg = MatchRegisterName(Tok.getIdentifier());
  if (Reg == GPUMin::NoRegister)
    return ParseStatus::NoMatch;

  StartLoc = Tok.getLoc();
  EndLoc = Tok.getEndLoc();
  Lex();
  return ParseStatus::Success;
}

bool GPUMinAsmParser::parseRegister(MCRegister &Reg, SMLoc &StartLoc,
                                    SMLoc &EndLoc) {
  ParseStatus Res = tryParseRegister(Reg, StartLoc, EndLoc);
  if (Res.isFailure())
    return Error(StartLoc, "invalid register name");
  if (Res.isSuccess())
    return false;
  return true;
}

bool GPUMinAsmParser::parseOperand(OperandVector &Operands, StringRef Mnemonic) {
  MCRegister Reg;
  SMLoc StartLoc, EndLoc;
  if (tryParseRegister(Reg, StartLoc, EndLoc).isSuccess()) {
    Operands.push_back(GPUMinOperand::createReg(Reg, StartLoc, EndLoc));
    return false;
  }

  // `(r2)` is a memory operand with a zero displacement.
  if (getLexer().is(AsmToken::LParen)) {
    if (parseMemOpBaseReg(Operands).isFailure())
      return true;
    return false;
  }

  // Otherwise a plain expression.  `8(r2)` produces two operands: the offset
  // expression and the base register.
  SMLoc S = getLoc();
  const MCExpr *Expr;
  if (getParser().parseExpression(Expr))
    return true;
  Operands.push_back(GPUMinOperand::createImm(Expr, S, getLoc()));

  if (getLexer().is(AsmToken::LParen)) {
    if (parseMemOpBaseReg(Operands).isFailure())
      return Error(getLoc(), "expected memory base register");
  }
  return false;
}

ParseStatus GPUMinAsmParser::parseMemOpBaseReg(OperandVector &Operands) {
  // The matcher compares these literal tokens against the '(' and ')' in the
  // instruction's AsmString (for example `$imm($rs1)`), so they are part of
  // the operand vector.
  if (parseToken(AsmToken::LParen, "expected '('"))
    return ParseStatus::Failure;
  Operands.push_back(GPUMinOperand::createToken("(", getLoc()));

  MCRegister Reg;
  SMLoc StartLoc, EndLoc;
  if (tryParseRegister(Reg, StartLoc, EndLoc).isNoMatch())
    return Error(getLoc(), "expected register");
  Operands.push_back(GPUMinOperand::createReg(Reg, StartLoc, EndLoc));

  if (parseToken(AsmToken::RParen, "expected ')'"))
    return ParseStatus::Failure;
  Operands.push_back(GPUMinOperand::createToken(")", getLoc()));

  return ParseStatus::Success;
}

bool GPUMinAsmParser::parseInstruction(ParseInstructionInfo &Info,
                                       StringRef Name, SMLoc NameLoc,
                                       OperandVector &Operands) {
  Operands.push_back(GPUMinOperand::createToken(Name, NameLoc));

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

bool GPUMinAsmParser::matchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                                              OperandVector &Operands,
                                              MCStreamer &Out,
                                              uint64_t &ErrorInfo,
                                              bool MatchingInlineAsm) {
  MCInst Inst;
  FeatureBitset MissingFeatures;
  unsigned MatchResult = MatchInstructionImpl(
      Operands, Inst, ErrorInfo, MissingFeatures, MatchingInlineAsm);

  switch (MatchResult) {
  case Match_Success:
    Inst.setLoc(IDLoc);
    Out.emitInstruction(Inst, *STI);
    return false;
  case Match_MissingFeature:
    return Error(IDLoc, "instruction is not available on this subtarget");
  case Match_MnemonicFail:
    return Error(IDLoc, "unrecognized instruction mnemonic");
  case Match_InvalidOperand: {
    SMLoc ErrorLoc = IDLoc;
    if (ErrorInfo != ~0U) {
      if (ErrorInfo >= Operands.size())
        return Error(IDLoc, "too few operands for instruction");
      ErrorLoc = Operands[ErrorInfo]->getStartLoc();
      if (ErrorLoc == SMLoc())
        ErrorLoc = IDLoc;
    }
    return Error(ErrorLoc, "invalid operand for instruction");
  }
  default:
    break;
  }
  llvm_unreachable("Unhandled match result");
}

ParseStatus GPUMinAsmParser::parseDirective(AsmToken DirectiveID) {
  // GM32 has no target-specific directives yet; chapter 01 adds .reg syntax
  // helpers if the tutorial needs them.
  return ParseStatus::NoMatch;
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeGPUMinAsmParser() {
  RegisterMCAsmParser<GPUMinAsmParser> X(getTheGPUMinTarget());
}

#define GET_REGISTER_MATCHER
#define GET_MATCHER_IMPLEMENTATION
#include "GPUMinGenAsmMatcher.inc"
