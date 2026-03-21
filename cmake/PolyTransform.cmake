# =============================================================================
# PolyTransform.cmake - Acquire the poly-transform tool
# =============================================================================
# Acquires poly-transform (in order of preference):
#   1. Pre-installed on PATH
#   2. Built from in-tree source (tools/poly-transform)
#
# The tool generates random per-build instruction subsets and verifies that
# compiled shellcode uses only the allowed instruction vocabulary.  Combined
# with build-unique DJB2 seeding, this makes static instruction-pattern
# signatures effectively impossible.
#
# Outputs:
#   POLY_TRANSFORM_EXECUTABLE  - Path to poly-transform binary
#   POLY_TRANSFORM_TARGET      - CMake target to add as build dependency
#   POLY_TRANSFORM_SEED        - FNV-1a seed derived from build timestamp

include_guard(GLOBAL)

# Platform-specific binary name
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    set(_ply_bin_name "poly-transform.exe")
else()
    set(_ply_bin_name "poly-transform")
endif()

pir_log_debug_at("poly-transform" "Host: ${CMAKE_HOST_SYSTEM_NAME}, binary=${_ply_bin_name}")

# -----------------------------------------------------------------------------
# Helper: set output variables and return
# -----------------------------------------------------------------------------
macro(_ply_found path)
    set(POLY_TRANSFORM_EXECUTABLE "${path}")
    set(POLY_TRANSFORM_TARGET "")
    pir_log_at("poly-transform" "${ARGN}")
    pir_log_verbose_at("poly-transform" "Path: ${path}")
    return()
endmacro()

# Helper: find a built artifact in both single-config and multi-config layouts
macro(_ply_find_artifact var name build_dir)
    set(${var} "")
    if(EXISTS "${build_dir}/${name}")
        set(${var} "${build_dir}/${name}")
    elseif(EXISTS "${build_dir}/Release/${name}")
        set(${var} "${build_dir}/Release/${name}")
    endif()
    pir_log_debug_at("poly-transform" "Artifact search: ${name} in ${build_dir} -> ${${var}}")
endmacro()

# -----------------------------------------------------------------------------
# Generate seed from build timestamp (mirrors DJB2 FNV-1a seeding)
# -----------------------------------------------------------------------------
string(TIMESTAMP _ply_timestamp "%Y-%m-%d" UTC)
# Hash the date string via CMake MD5 and extract first 16 hex chars as seed
string(MD5 _ply_date_hash "${_ply_timestamp}")
string(SUBSTRING "${_ply_date_hash}" 0 16 _ply_seed_hex)
math(EXPR POLY_TRANSFORM_SEED "0x${_ply_seed_hex}" OUTPUT_FORMAT HEXADECIMAL)

pir_log_verbose_at("poly-transform" "Build date: ${_ply_timestamp}")
pir_log_verbose_at("poly-transform" "Seed: ${POLY_TRANSFORM_SEED}")

# =============================================================================
# Strategy 1: Pre-installed on PATH
# =============================================================================
pir_log_debug_at("poly-transform" "Strategy 1: Searching PATH...")
find_program(_ply_system_bin poly-transform)

if(_ply_system_bin)
    _ply_found("${_ply_system_bin}" "Using system binary")
endif()
pir_log_verbose_at("poly-transform" "Not found on PATH, trying in-tree build")

# =============================================================================
# Strategy 2: Build from in-tree source
# =============================================================================
pir_log_debug_at("poly-transform" "Strategy 2: Building from in-tree source...")

set(_ply_source_dir "${PIR_ROOT_DIR}/tools/poly-transform")

if(NOT EXISTS "${_ply_source_dir}/CMakeLists.txt")
    pir_log_at("poly-transform" "Source not found — tool disabled")
    set(POLY_TRANSFORM_EXECUTABLE "")
    set(POLY_TRANSFORM_TARGET "")
    return()
endif()
pir_log_debug_at("poly-transform" "Source found: ${_ply_source_dir}")

# ── Configure ───────────────────────────────────────────────────────────
set(_ply_build_dir "${CMAKE_BINARY_DIR}/poly-transform-build")

if(NOT EXISTS "${_ply_build_dir}/CMakeCache.txt")
    pir_log_at("poly-transform" "Configuring...")
    pir_log_verbose_at("poly-transform" "Source: ${_ply_source_dir}")
    pir_log_verbose_at("poly-transform" "Build dir: ${_ply_build_dir}")

    # Use the host compiler (not cross-compiler) for the tool
    get_filename_component(_ply_compiler_real "${CMAKE_CXX_COMPILER}" REALPATH)
    get_filename_component(_ply_compiler_dir  "${_ply_compiler_real}" DIRECTORY)

    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S "${_ply_source_dir}"
            -B "${_ply_build_dir}"
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_C_COMPILER=${_ply_compiler_dir}/clang
            -DCMAKE_CXX_COMPILER=${_ply_compiler_dir}/clang++
            -DSTATIC_LINK=ON
        RESULT_VARIABLE _ply_cfg_rc
        OUTPUT_VARIABLE _ply_cfg_out
        ERROR_VARIABLE  _ply_cfg_err
    )
    if(NOT _ply_cfg_rc EQUAL 0)
        pir_log_at("poly-transform" "Configure failed (rc=${_ply_cfg_rc}) — tool disabled")
        pir_log_debug_at("poly-transform" "${_ply_cfg_err}")
        set(POLY_TRANSFORM_EXECUTABLE "")
        set(POLY_TRANSFORM_TARGET "")
        return()
    endif()
else()
    pir_log_debug_at("poly-transform" "Reusing cached build dir: ${_ply_build_dir}")
endif()

# ── Build ───────────────────────────────────────────────────────────────
pir_log_at("poly-transform" "Building from source...")
string(TIMESTAMP _ply_build_start "%s")
execute_process(
    COMMAND ${CMAKE_COMMAND} --build "${_ply_build_dir}" --config Release
    RESULT_VARIABLE _ply_build_rc
    OUTPUT_VARIABLE _ply_build_out
    ERROR_VARIABLE  _ply_build_err
)
string(TIMESTAMP _ply_build_end "%s")
math(EXPR _ply_build_elapsed "${_ply_build_end} - ${_ply_build_start}")

if(NOT _ply_build_rc EQUAL 0)
    pir_log_at("poly-transform" "Build failed (rc=${_ply_build_rc}, ${_ply_build_elapsed}s) — tool disabled")
    pir_log_debug_at("poly-transform" "${_ply_build_err}")
    set(POLY_TRANSFORM_EXECUTABLE "")
    set(POLY_TRANSFORM_TARGET "")
    return()
endif()
pir_log_at("poly-transform" "Build succeeded (${_ply_build_elapsed}s)")

# ── Locate artifact ─────────────────────────────────────────────────────
_ply_find_artifact(_ply_bin "${_ply_bin_name}" "${_ply_build_dir}")
if(_ply_bin)
    _ply_found("${_ply_bin}" "Built from source")
endif()

pir_log_at("poly-transform" "Build succeeded but binary not found — tool disabled")
set(POLY_TRANSFORM_EXECUTABLE "")
set(POLY_TRANSFORM_TARGET "")
