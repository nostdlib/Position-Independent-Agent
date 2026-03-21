# =============================================================================
# PolyTransformAnalyze.cmake - Post-build instruction set analysis
# =============================================================================
# Generates disassembly via llvm-objdump and runs poly-transform analyze.
#
# Input variables:
#   PLY_OBJDUMP      - Path to llvm-objdump
#   PLY_EXECUTABLE   - Path to poly-transform
#   PLY_INPUT        - Input binary (.elf/.exe)
#   PLY_DISASM       - Output disassembly file
#   PLY_ARCH         - Target architecture
#   PLY_SEED         - Instruction set seed

# Normalize arch name for poly-transform (armv7a → armv7)
set(_ply_arch "${PLY_ARCH}")
if(_ply_arch STREQUAL "armv7a")
    set(_ply_arch "armv7")
endif()

# Generate disassembly
message(STATUS "[pir:poly-transform] Generating disassembly for ${PLY_ARCH}...")
execute_process(
    COMMAND "${PLY_OBJDUMP}" -d --no-addresses --no-show-raw-insn "${PLY_INPUT}"
    OUTPUT_FILE "${PLY_DISASM}"
    ERROR_VARIABLE _ply_objdump_err
    RESULT_VARIABLE _ply_objdump_rc
)

if(NOT _ply_objdump_rc EQUAL 0)
    message(STATUS "[pir:poly-transform] llvm-objdump failed (rc=${_ply_objdump_rc}) — skipping analysis")
    return()
endif()

# Run analysis
message(STATUS "[pir:poly-transform] Analyzing instruction usage...")
execute_process(
    COMMAND "${PLY_EXECUTABLE}" analyze --arch "${_ply_arch}" --disasm "${PLY_DISASM}"
    OUTPUT_VARIABLE _ply_analysis_out
    ERROR_VARIABLE  _ply_analysis_err
    RESULT_VARIABLE _ply_analysis_rc
)

if(_ply_analysis_rc EQUAL 0)
    message(STATUS "${_ply_analysis_out}")
else()
    message(STATUS "[pir:poly-transform] Analysis failed (rc=${_ply_analysis_rc})")
endif()

# Run instruction set generation (informational)
execute_process(
    COMMAND "${PLY_EXECUTABLE}" generate --arch "${_ply_arch}" --seed "${PLY_SEED}"
    OUTPUT_VARIABLE _ply_generate_out
    RESULT_VARIABLE _ply_generate_rc
)

if(_ply_generate_rc EQUAL 0)
    message(STATUS "${_ply_generate_out}")
endif()
