# =============================================================================
# Target.cmake - Platform Selection, Target Definition, and Post-Build
# =============================================================================

include_guard(GLOBAL)

# =============================================================================
# Platform Modules
# =============================================================================
set(_platform_map_windows Windows)
set(_platform_map_linux Linux)
set(_platform_map_macos macOS)
set(_platform_map_uefi UEFI)
set(_platform_map_solaris Solaris)
set(_platform_map_freebsd FreeBSD)
set(_platform_map_android Android)
set(_platform_map_ios iOS)
pir_log_verbose("Loading platform module: ${_platform_map_${PIR_PLATFORM}}.cmake")
include(${PIR_ROOT_DIR}/cmake/platforms/${_platform_map_${PIR_PLATFORM}}.cmake)

# Log final source count after platform filtering
list(LENGTH PIR_SOURCES _post_filter_src)
list(LENGTH PIR_HEADERS _post_filter_hdr)
pir_log_verbose("After platform filtering: ${_post_filter_src} sources, ${_post_filter_hdr} headers")

# Universal target flags (every platform uses -target for both compile and link)
list(APPEND PIR_BASE_FLAGS -target ${PIR_TRIPLE})
if(NOT PIR_DIRECT_LINKER)
    if(DEFINED PIR_LINK_TRIPLE)
        list(APPEND PIR_BASE_LINK_FLAGS -target ${PIR_LINK_TRIPLE})
    else()
        list(APPEND PIR_BASE_LINK_FLAGS -target ${PIR_TRIPLE})
    endif()
endif()

# =============================================================================
# Output Configuration
# =============================================================================
file(MAKE_DIRECTORY "${PIR_OUTPUT_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PIR_OUTPUT_DIR}")
set(CMAKE_PDB_OUTPUT_DIRECTORY "${PIR_OUTPUT_DIR}")

# =============================================================================
# Target
# =============================================================================
add_executable(${PIR_TRIPLE} ${PIR_SOURCES} ${PIR_HEADERS})

set_target_properties(${PIR_TRIPLE} PROPERTIES
    OUTPUT_NAME "output"
    SUFFIX "${PIR_EXT}"
)

target_include_directories(${PIR_TRIPLE} PRIVATE ${PIR_INCLUDE_PATHS})
target_compile_definitions(${PIR_TRIPLE} PRIVATE ${PIR_DEFINES})
target_compile_options(${PIR_TRIPLE} PRIVATE ${PIR_BASE_FLAGS})
target_link_options(${PIR_TRIPLE} PRIVATE ${PIR_BASE_LINK_FLAGS})

# ── pic-transform integration ────────────────────────────────────────────────
# Use -fpass-plugin= (if plugin available) or compile-to-bitcode + standalone
# tool + bitcode-to-object pipeline.
if(PIC_TRANSFORM_PLUGIN)
    target_compile_options(${PIR_TRIPLE} PRIVATE
        "SHELL:-fpass-plugin=${PIC_TRANSFORM_PLUGIN}")
    pir_log_verbose_at("pic-transform" "Mode: plugin (integrated into compiler pipeline)")
    pir_log_verbose_at("pic-transform" "Plugin: ${PIC_TRANSFORM_PLUGIN}")
elseif(PIC_TRANSFORM_EXECUTABLE)
    # Standalone mode: emit LLVM bitcode, transform, then compile to object.
    # Override the compile rule with a 3-step pipeline:
    #   1. clang++ -emit-llvm -c <source> -o <object>.bc
    #   2. pic-transform <object>.bc -o <object>.transformed.bc
    #   3. clang++ -c <object>.transformed.bc -o <object>
    # Use cmd /C on Windows for && chaining; Unix shells handle it natively.
    if(CMAKE_HOST_WIN32)
        set(CMAKE_CXX_COMPILE_OBJECT
            "cmd /C \"<CMAKE_CXX_COMPILER> <DEFINES> <INCLUDES> <FLAGS> -emit-llvm -c <SOURCE> -o <OBJECT>.bc && ${PIC_TRANSFORM_EXECUTABLE} <OBJECT>.bc -o <OBJECT>.transformed.bc && <CMAKE_CXX_COMPILER> -Wno-unused-command-line-argument <FLAGS> -c <OBJECT>.transformed.bc -o <OBJECT>\""
        )
    else()
        set(CMAKE_CXX_COMPILE_OBJECT
            "<CMAKE_CXX_COMPILER> <DEFINES> <INCLUDES> <FLAGS> -emit-llvm -c <SOURCE> -o <OBJECT>.bc && ${PIC_TRANSFORM_EXECUTABLE} <OBJECT>.bc -o <OBJECT>.transformed.bc && <CMAKE_CXX_COMPILER> -Wno-unused-command-line-argument <FLAGS> -c <OBJECT>.transformed.bc -o <OBJECT>"
        )
    endif()
    pir_log_verbose_at("pic-transform" "Mode: standalone (3-step compile pipeline)")
    pir_log_verbose_at("pic-transform" "Executable: ${PIC_TRANSFORM_EXECUTABLE}")
else()
    pir_log_verbose_at("pic-transform" "Mode: disabled (no plugin or executable found)")
endif()

# Ensure pic-transform is built before the main target
if(PIC_TRANSFORM_TARGET)
    add_dependencies(${PIR_TRIPLE} ${PIC_TRANSFORM_TARGET})
endif()

# =============================================================================
# Post-Build
# =============================================================================
pir_add_postbuild(${PIR_TRIPLE})

if(PIR_PLATFORM STREQUAL "uefi")
    pir_add_uefi_boot(${PIR_TRIPLE})
endif()

# =============================================================================
# Build Summary
# =============================================================================

# Detect compiler version for summary
execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" --version
    OUTPUT_VARIABLE _compiler_version_raw
    ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" _compiler_version "${_compiler_version_raw}")
if(NOT _compiler_version)
    set(_compiler_version "unknown")
endif()

# Source/header counts (post-filtering)
list(LENGTH PIR_SOURCES _final_src_count)
list(LENGTH PIR_HEADERS _final_hdr_count)

# pic-transform mode string
if(PIC_TRANSFORM_PLUGIN)
    set(_pt_mode "plugin")
elseif(PIC_TRANSFORM_EXECUTABLE)
    set(_pt_mode "standalone")
else()
    set(_pt_mode "disabled")
endif()

pir_log_header("Build Configuration")
pir_log_kv("Platform"      "${PIR_PLATFORM}")
pir_log_kv("Architecture"  "${PIR_ARCH}")
pir_log_kv("Build type"    "${PIR_BUILD_TYPE}")
pir_log_kv("Optimization"  "-${PIR_OPT_LEVEL}")
pir_log_kv("Triple"        "${PIR_TRIPLE}")
pir_log_kv("Compiler"      "clang ${_compiler_version}")
pir_log_kv("Sources"       "${_final_src_count} sources, ${_final_hdr_count} headers")
pir_log_kv("pic-transform" "${_pt_mode}")
if(POLY_TRANSFORM_EXECUTABLE)
    pir_log_kv("poly-transform" "enabled (seed=${POLY_TRANSFORM_SEED})")
else()
    pir_log_kv("poly-transform" "disabled")
endif()
pir_log_kv("Logging"       "${ENABLE_LOGGING}")
pir_log_kv("Output"        "${PIR_OUTPUT_DIR}/output${PIR_EXT}")
pir_log_kv("App layer"     "$<IF:$<BOOL:${BUILD_TESTS}>,tests,beacon>")
pir_log_footer()

# Verbose: host system info and toolchain details
pir_log_verbose("Host: ${CMAKE_HOST_SYSTEM_NAME} ${CMAKE_HOST_SYSTEM_PROCESSOR}")
pir_log_verbose("CMake: ${CMAKE_VERSION}")
pir_log_verbose("Generator: ${CMAKE_GENERATOR}")
pir_log_verbose("Log level: ${PIR_LOG_LEVEL}")

# Debug: full flag lists for troubleshooting
pir_log_debug("Compile flags: ${PIR_BASE_FLAGS}")
pir_log_debug("Link flags: ${PIR_BASE_LINK_FLAGS}")
pir_log_debug("Defines: ${PIR_DEFINES}")
if(PIR_DIRECT_LINKER)
    pir_log_debug("Direct linker: ${CMAKE_LINKER}")
endif()
