#===----------------------------------------------------------------------===//
# AutoTD.cmake - wire the auto-td instruction-record generator into a target
#
# A target that keeps its instruction corpus under <target>/auto-td/ calls
# llvm_autotd_generate() once.  The function defines the custom command that
# produces AutoTdInstrInfo.inc and registers the corpus so that the single
# check-autotd target validates every corpus in the build.
#===----------------------------------------------------------------------===//

include_guard(GLOBAL)

# llvm_autotd_generate(<name>
#   TARGET_ROOT <dir>          # directory containing auto-td/
#   RISCV_OPCODES <dir>        # upstream opcode database
#   [YSX_OPCODES <dir>]        # vendor opcode overlay
#   [OUT_DIR <dir>]            # default: <current binary dir>/auto-td
#   [COVERAGE <file>]          # default: <OUT_DIR>/AutoTdCoverage.md
#   [RECORD_PREFIX <prefix>]   # default: config.yaml's record_prefix
#   [EXTRA_DEPENDS <file>...]  # additional inputs of the custom command
#   [EXTRA_ARGS <arg>...])     # additional command-line arguments
#
# On return <name>_AUTOTD_INSTRINFO names the generated instruction file; a
# target normally does set(LLVM_TARGET_DEPENDS ${<name>_AUTOTD_INSTRINFO}).
function(llvm_autotd_generate name)
  cmake_parse_arguments(ARG ""
    "TARGET_ROOT;RISCV_OPCODES;YSX_OPCODES;OUT_DIR;COVERAGE;RECORD_PREFIX"
    "EXTRA_DEPENDS;EXTRA_ARGS"
    ${ARGN})

  if(NOT ARG_TARGET_ROOT OR NOT ARG_RISCV_OPCODES)
    message(FATAL_ERROR "llvm_autotd_generate(${name}): TARGET_ROOT and RISCV_OPCODES are required")
  endif()
  if(NOT ARG_OUT_DIR)
    set(ARG_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/auto-td)
  endif()
  if(NOT ARG_COVERAGE)
    set(ARG_COVERAGE ${ARG_OUT_DIR}/AutoTdCoverage.md)
  endif()

  set(generator ${LLVM_MAIN_SRC_DIR}/utils/auto-td/auto-td-gen)
  file(GLOB_RECURSE inputs CONFIGURE_DEPENDS
       ${ARG_TARGET_ROOT}/auto-td/*.yaml
       ${ARG_RISCV_OPCODES}/arg_lut.csv
       ${ARG_RISCV_OPCODES}/extensions/*
       ${LLVM_MAIN_SRC_DIR}/utils/auto-td/autotd/*.py)
  if(ARG_YSX_OPCODES)
    file(GLOB vendor_inputs CONFIGURE_DEPENDS ${ARG_YSX_OPCODES}/extensions/*)
    list(APPEND inputs ${vendor_inputs})
  endif()

  set(instrinfo ${ARG_OUT_DIR}/AutoTdInstrInfo.inc)
  set(manifests ${ARG_OUT_DIR}/AutoTdManifests.inc)

  set(args
    --target-root ${ARG_TARGET_ROOT}
    --riscv-opcodes ${ARG_RISCV_OPCODES}
    --out-dir ${ARG_OUT_DIR}
    --coverage ${ARG_COVERAGE})
  if(ARG_YSX_OPCODES)
    list(APPEND args --ysx-opcodes ${ARG_YSX_OPCODES})
  endif()
  if(ARG_RECORD_PREFIX)
    list(APPEND args --record-prefix ${ARG_RECORD_PREFIX})
  endif()
  list(APPEND args ${ARG_EXTRA_ARGS})

  add_custom_command(
    OUTPUT ${instrinfo} ${manifests} ${ARG_COVERAGE}
    COMMAND ${Python3_EXECUTABLE} ${generator} ${args}
    DEPENDS ${inputs} ${ARG_EXTRA_DEPENDS} ${generator}
    COMMENT "Generating ${name} instruction records with auto-td"
    VERBATIM)

  set(${name}_AUTOTD_INSTRINFO ${instrinfo} PARENT_SCOPE)

  # Remember the corpus for check-autotd.  Fields are '|'-separated because a
  # global property is a flat list; no path in a build contains '|'.
  set_property(GLOBAL APPEND PROPERTY LLVM_AUTOTD_CORPORA
    "${name}|${ARG_TARGET_ROOT}|${ARG_RISCV_OPCODES}|${ARG_YSX_OPCODES}|${ARG_OUT_DIR}")
endfunction()

# llvm_autotd_add_check_target()
#
# One target that validates every registered corpus (--check-only: field
# coverage, predicates, duplicate rows, coverage and reference gates) and runs
# the generator's unit tests.
function(llvm_autotd_add_check_target)
  get_property(corpora GLOBAL PROPERTY LLVM_AUTOTD_CORPORA)
  if(NOT corpora)
    return()
  endif()

  set(commands)
  foreach(entry IN LISTS corpora)
    string(REPLACE "|" ";" fields "${entry}")
    list(GET fields 0 name)
    list(GET fields 1 target_root)
    list(GET fields 2 riscv_opcodes)
    list(GET fields 3 ysx_opcodes)
    list(GET fields 4 out_dir)
    set(args
      --target-root ${target_root}
      --riscv-opcodes ${riscv_opcodes}
      --out-dir ${CMAKE_BINARY_DIR}/autotd-check/${name}
      --check-only)
    if(ysx_opcodes)
      list(APPEND args --ysx-opcodes ${ysx_opcodes})
    endif()
    list(APPEND commands
      COMMAND ${Python3_EXECUTABLE} ${LLVM_MAIN_SRC_DIR}/utils/auto-td/auto-td-gen ${args})
  endforeach()

  add_custom_target(check-autotd
    ${commands}
    COMMAND ${Python3_EXECUTABLE} -m unittest discover
            -s ${LLVM_MAIN_SRC_DIR}/utils/auto-td/tests -p test_*.py
    COMMENT "Checking auto-td corpora and unit tests"
    USES_TERMINAL)
endfunction()
