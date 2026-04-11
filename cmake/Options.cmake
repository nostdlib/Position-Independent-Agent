# =============================================================================
# Options.cmake - Build Option Validation and Derived Settings
# =============================================================================

include_guard(GLOBAL)

# =============================================================================
# Dependency Scanning
# =============================================================================
# Required because CMAKE_CXX_COMPILER_FORCED skips compiler detection,
# which includes dependency scanning setup. This ensures header changes
# trigger rebuilds.
set(CMAKE_DEPENDS_USE_COMPILER TRUE)

# =============================================================================
# Build Options
# =============================================================================
set(ARCHITECTURE "x86_64" CACHE STRING "Target: i386, x86_64, armv7a, aarch64")
set(PLATFORM "windows" CACHE STRING "Target: windows, linux, uefi")
set(BUILD_TYPE "release" CACHE STRING "Build type: debug, release")
set(OPTIMIZATION_LEVEL "" CACHE STRING "Override optimization level (e.g., O2, Os)")
option(BUILD_TESTS "Build tests instead of beacon" OFF)
option(ENABLE_LOGGING "Enable logging macros" ON)
option(NO_SYSCALL "Use direct ntdll exports instead of indirect syscalls (Windows only)" OFF)

# Normalize inputs
string(TOLOWER "${ARCHITECTURE}" PIR_ARCH)
string(TOLOWER "${PLATFORM}" PIR_PLATFORM)
string(TOLOWER "${BUILD_TYPE}" PIR_BUILD_TYPE)

# Validate inputs
set(_valid_archs i386 x86_64 armv7a aarch64 riscv32 riscv64 mips64)
set(_valid_platforms windows linux macos uefi solaris freebsd android ios)

if(NOT PIR_ARCH IN_LIST _valid_archs)
    message(FATAL_ERROR "[pir] Invalid ARCHITECTURE '${ARCHITECTURE}'. Valid: ${_valid_archs}")
endif()
if(NOT PIR_PLATFORM IN_LIST _valid_platforms)
    message(FATAL_ERROR "[pir] Invalid PLATFORM '${PLATFORM}'. Valid: ${_valid_platforms}")
endif()
if(NOT PIR_BUILD_TYPE MATCHES "^(debug|release)$")
    message(FATAL_ERROR "[pir] Invalid BUILD_TYPE '${BUILD_TYPE}'. Valid: debug, release")
endif()

# Resolve application layer directory
if(BUILD_TESTS)
    set(PIR_APP_DIR "${CMAKE_SOURCE_DIR}/tests")
    pir_log_debug("App layer: tests")
else()
    set(PIR_APP_DIR "${CMAKE_SOURCE_DIR}/src/beacon")
    pir_log_debug("App layer: beacon")
endif()

# Derived settings
string(TOUPPER "${PIR_ARCH}" PIR_ARCH_UPPER)
string(TOUPPER "${PIR_PLATFORM}" PIR_PLATFORM_UPPER)
set(PIR_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/build/${PIR_BUILD_TYPE}/${PIR_PLATFORM}/${PIR_ARCH}")
set(PIR_MAP_FILE "${PIR_OUTPUT_DIR}/output.map.txt")
set(PIR_IS_DEBUG $<STREQUAL:${PIR_BUILD_TYPE},debug>)

if(OPTIMIZATION_LEVEL)
    set(PIR_OPT_LEVEL "${OPTIMIZATION_LEVEL}")
    pir_log_debug("Optimization: user override -${PIR_OPT_LEVEL}")
elseif(PIR_BUILD_TYPE STREQUAL "debug")
    set(PIR_OPT_LEVEL "Og")
    pir_log_debug("Optimization: debug default -Og")
else()
    set(PIR_OPT_LEVEL "Oz")
    pir_log_debug("Optimization: release default -Oz")
endif()

# =============================================================================
# Preprocessor Defines
# =============================================================================
set(PIR_DEFINES
    ARCHITECTURE_${PIR_ARCH_UPPER}
    PLATFORM_${PIR_PLATFORM_UPPER}
    PLATFORM_${PIR_PLATFORM_UPPER}_${PIR_ARCH_UPPER}
)
if(PIR_BUILD_TYPE STREQUAL "debug")
    list(APPEND PIR_DEFINES DEBUG)
endif()
if(ENABLE_LOGGING)
    list(APPEND PIR_DEFINES ENABLE_LOGGING)
endif()
if(NO_SYSCALL)
    if(NOT PIR_PLATFORM STREQUAL "windows")
        message(FATAL_ERROR "[pir] NO_SYSCALL is only meaningful on PLATFORM=windows (got '${PIR_PLATFORM}')")
    endif()
    list(APPEND PIR_DEFINES NO_SYSCALL)
endif()

# Log resolved options at verbose level
pir_log_verbose("Options resolved: ${PIR_PLATFORM}/${PIR_ARCH} ${PIR_BUILD_TYPE} -${PIR_OPT_LEVEL}")
pir_log_debug("Defines: ${PIR_DEFINES}")
pir_log_debug("Output dir: ${PIR_OUTPUT_DIR}")
