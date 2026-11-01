// RUN: llvm-tblgen -gen-subtarget -I %p/../../include %s -o - | FileCheck %s
// RUN: not llvm-tblgen -gen-subtarget -DERROR -I %p/../../include %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERROR

// Check that instregex results are cached per pattern without changing
// semantics: the same pattern used by different processor models must select
// the same instructions, while a pattern that differs only in anchoring must
// not reuse the cached result.

include "llvm/Target/Target.td"

def TestTargetInstrInfo : InstrInfo;
def TestTarget : Target { let InstructionSet = TestTargetInstrInfo; }

def InstA  : Instruction { let OutOperandList = (outs); let InOperandList = (ins); }
def InstAB : Instruction { let OutOperandList = (outs); let InOperandList = (ins); }

def ResA : ProcResource<1>;

def ModelA : SchedMachineModel { let CompleteModel = false; }
def ModelB : SchedMachineModel { let CompleteModel = false; }
def ModelC : SchedMachineModel { let CompleteModel = false; }

let SchedModel = ModelA in
def WriteA : SchedWriteRes<[ResA]>;
let SchedModel = ModelB in
def WriteB : SchedWriteRes<[ResA]>;
let SchedModel = ModelC in
def WriteC : SchedWriteRes<[ResA]>;

def ProcA : ProcessorModel<"proca", ModelA, []>;
def ProcB : ProcessorModel<"procb", ModelB, []>;
def ProcC : ProcessorModel<"procc", ModelC, []>;

// ModelA's unanchored pattern matches both InstA and InstAB.
let SchedModel = ModelA in
def : InstRW<[WriteA], (instregex "InstA")>;

// ModelB anchors the pattern, so only InstA may match.
let SchedModel = ModelB in
def : InstRW<[WriteB], (instregex "^InstA$")>;

// ModelC repeats ModelA's pattern and must reuse the cached result.
let SchedModel = ModelC in
def : InstRW<[WriteC], (instregex "InstA")>;

#ifdef ERROR
let SchedModel = ModelC in
def : InstRW<[WriteC], (instregex "NoSuchInstruction")>;
#endif

// CHECK: static const llvm::MCSchedClassDesc ModelASchedClasses[] = {
// CHECK: {DBGFIELD(/*InstA_InstAB*/ {{[0-9]+}}) 1, false, false, false,
// CHECK: {DBGFIELD(/*InstA*/ {{[0-9]+}}) 1, false, false, false,

// CHECK: static const llvm::MCSchedClassDesc ModelBSchedClasses[] = {
// CHECK: {DBGFIELD(/*InstA_InstAB*/ {{[0-9]+}}) 8191, false, false, false,
// CHECK: {DBGFIELD(/*InstA*/ {{[0-9]+}}) 1, false, false, false,

// CHECK: static const llvm::MCSchedClassDesc ModelCSchedClasses[] = {
// CHECK: {DBGFIELD(/*InstA_InstAB*/ {{[0-9]+}}) 1, false, false, false,
// CHECK: {DBGFIELD(/*InstA*/ {{[0-9]+}}) 1, false, false, false,

// ERROR: error: instregex has no matches: NoSuchInstruction
