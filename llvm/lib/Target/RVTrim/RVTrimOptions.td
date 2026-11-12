//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

include "llvm/Option/LibraryOptions.td"

def RVTrimOptions : OptionsStruct<prefix = "rvtrim-">;

// RVTrimISelDAGToDAG.cpp
defm : BoolField<"rvtrim-use-rematerializable-movimm", "false",
  "Use a rematerializable pseudoinstruction for 2 instruction constant materialization">;

// RVTrimISelLowering.cpp
defm : ValueField<"rvtrim-br-merging-base-cost", "std::optional<int>", "std::nullopt",
  "Sets the cost threshold for when multiple conditionals will be merged into one branch versus be split in multiple branches. Merging conditionals saves branches at the cost of additional instructions. This value sets the instruction cost limit, below which conditionals will be merged, and above which conditionals will be split. Set to -1 to never merge branches">;
defm : ValueField<"rvtrim-br-merging-likely-bias", "int", "0",
  "Increases 'rvtrim-br-merging-base-cost' in cases that it is likely that all conditionals will be executed. For example for merging the conditionals (a == b && c > d), if its known that a == b is likely, then it is likely that if the conditionals are split both sides will be executed, so it may be desirable to increase the instruction cost threshold. Set to -1 to never merge likely branches">;
defm : ValueField<"rvtrim-br-merging-unlikely-bias", "int", "-1",
  "Decreases 'rvtrim-br-merging-base-cost' in cases that it is unlikely that all conditionals will be executed. For example for merging the conditionals (a == b && c > d), if its known that a == b is unlikely, then it is unlikely that if the conditionals are split both sides will be executed, so it may be desirable to decrease the instruction cost threshold. Set to -1 to never merge unlikely branches">;
defm : ValueField<"rvtrim-lower-ext-max-web-size", "unsigned", "18",
  "Give the maximum size (in number of nodes) of the web of instructions that we will consider for VW expansion">;
defm : BoolField<"rvtrim-lower-form-vw-w-with-splat", "false",
  "Allow the formation of VW_W operations (e.g., VWADD_W) with splat constants">;
defm : ValueField<"rvtrim-lower-fp-repeated-divisors", "unsigned", "2",
  "Set the minimum number of repetitions of a divisor to allow transformation to multiplications by the reciprocal">;
defm : ValueField<"rvtrim-lower-fpimm-cost", "int", "3",
  "Give the maximum number of instructions that we will use for creating a floating-point immediate value">;
defm : BoolField<"rvtrim-reassoc-shl-addi-add", "true",
  "Swap add and addi in cases where the add may be combined with a shift">;

// RVTrimIndirectBranchTracking.cpp
defm : ValueField<"rvtrim-landing-pad-label", "std::optional<uint32_t>", "std::nullopt",
  "Use preferred fixed label for all labels">;

// RVTrimInsertReadWriteCSR.cpp
defm : BoolField<"rvtrim-frm-insert-opt", "true",
  "Enable optimized frm insertion">;

// RVTrimInsertVSETVLI.cpp
defm : BoolField<"rvtrim-insert-vsetvli-whole-vector-register-move-valid-vtype", "true",
  "Insert vsetvlis before vmvNr.vs to ensure vtype is valid and vill is cleared">;

// RVTrimInstrInfo.cpp
defm : EnumField<"rvtrim-force-machine-combiner-strategy", "std::optional<MachineTraceStrategy>",
  "std::nullopt", "local,min-instr",
  ["MachineTraceStrategy::TS_Local", "MachineTraceStrategy::TS_MinInstrCount"],
  "Force machine combiner to use a specific strategy for machine trace metrics evaluation">;
defm : BoolField<"rvtrim-outliner-regsave", "true",
  "Enable RegSave strategy in machine outliner (save X5 to a temporary register when X5 is live across outlined calls)">;
defm : BoolField<"rvtrim-prefer-whole-register-move", "false",
  "Prefer whole register move for vector registers">;

// RVTrimLoadStoreOptimizer.cpp
// The limit on how many instructions we search for load/store pairs.
defm : ValueField<"rvtrim-load-store-scan-limit", "unsigned", "128", "">;

// RVTrimOptWInstrs.cpp
defm : BoolField<"rvtrim-sextw-removal", "true",
  "Enable removal of sext.w">;
defm : BoolField<"rvtrim-strip-w-suffix", "true",
  "Enable strip W suffix">;

// RVTrimRegisterInfo.cpp
defm : BoolField<"rvtrim-cost-per-use", "true", "">;
defm : BoolField<"rvtrim-regalloc-hints", "true",
  "Enable two address hints for register allocation">;

// RVTrimSubtarget.cpp
defm : BoolField<"rvtrim-constant-pool-for-large-ints", "true",
  "Enable using constant pool for large integers">;
defm : ValueField<"rvtrim-max-build-ints-cost", "unsigned", "0",
  "The maximum cost used for building integers">;
defm : ValueField<"rvtrim-min-jump-table-entries", "std::optional<unsigned>", "std::nullopt",
  "Set minimum number of entries to use a jump table on RVTrim">;
defm : BoolField<"rvtrim-mips-ccmov", "true",
  "Use 'mips.ccmov' instruction">;
defm : BoolField<"rvtrim-mips-load-store-pairs", "false",
  "Enable the load/store pair optimization pass">;
defm : ValueField<"rvtrim-sched-load-latency", "std::optional<unsigned>", "std::nullopt",
  "Override the load latency (in cycles) in the scheduler model. A non-negative value overrides the target default">;
defm : ValueField<"rvtrim-sched-mispredict-penalty", "std::optional<unsigned>", "std::nullopt",
  "Override the mispredict penalty (in cycles) in the scheduler model. A non-negative value overrides the target default">;
defm : BoolField<"rvtrim-use-aa", "true",
  "Enable the use of AA during codegen">;

// RVTrimTargetMachine.cpp
defm : BoolField<"rvtrim-enable-cfi-instr-inserter", "false",
  "Enable CFI Instruction Inserter for RISC-V">;
defm : BoolField<"rvtrim-enable-copy-propagation", "true",
  "Enable the copy propagation with RISC-V copy instr">;
defm : BoolField<"rvtrim-enable-copyelim", "true",
  "Enable the redundant copy elimination pass">;
defm : BoolField<"rvtrim-enable-dead-defs", "true",
  "Enable the pass that removes dead definitions and replaces stores to them with stores to x0">;
// FIXME: Unify control over GlobalMerge.
defm : OptionalBoolField<"rvtrim-enable-global-merge",
  "Enable the global merge pass">;
defm : BoolField<"rvtrim-enable-loop-data-prefetch", "true",
  "Enable the loop data prefetch pass">;
defm : BoolField<"rvtrim-enable-machine-combiner", "true",
  "Enable the machine combiner pass">;
defm : BoolField<"rvtrim-enable-sink-fold", "true",
  "Enable sinking and folding of instruction copies">;
defm : BoolField<"rvtrim-select-opt", "true",
  "Enable select to branch optimizations">;
defm : ValueField<"rvtrim-v-vector-bits-max", "std::optional<unsigned>", "std::nullopt",
  "Assume V extension vector registers are at most this big, with zero meaning no maximum size is assumed">;
defm : ValueField<"rvtrim-v-vector-bits-min", "std::optional<int>", "std::nullopt",
  "Assume V extension vector registers are at least this big, with zero meaning no minimum size is assumed. A value of -1 means use Zvl*b extension. This is primarily used to enable autovectorization with fixed width vectors">;
defm : BoolField<"rvtrim-vector-mask-mutation", "true",
  "Enable the vector mask scheduling mutation">;

// RVTrimTargetTransformInfo.cpp
defm : BoolField<"rvtrim-or-like-select", "true", "">;
defm : ValueField<"rvtrim-v-min-trip-count", "unsigned", "5",
  "Set the lower bound of a trip count to decide on vectorization while tail-folding">;
defm : ValueField<"rvtrim-v-register-bit-width-lmul", "unsigned", "2",
  "The LMUL to use for getRegisterBitWidth queries. Affects LMUL used by autovectorized code. Fractional LMULs are not supported">;
defm : ValueField<"rvtrim-v-slp-max-vf", "std::optional<unsigned>", "std::nullopt",
  "Overrides result used for getMaximumVF query which is used exclusively by SLP vectorizer">;

// RVTrimZilsdOptimizer.cpp
defm : ValueField<"rvtrim-zilsd-max-reschedule-distance", "unsigned", "10",
  "Maximum distance for rescheduling load/store instructions">;
defm : BoolField<"rvtrim-zilsd-opt", "true",
  "Enable Zilsd load/store optimization">;
