# =============================================================================
# Position-Independent Agent
# =============================================================================

cmake_minimum_required(VERSION 3.20)

option(BUILD_DOCS "Build documentation only (no compiler required)" OFF)

# =============================================================================
# Documentation-only build
# =============================================================================
if(BUILD_DOCS)
    project(pir-docs LANGUAGES NONE)
    include(${CMAKE_SOURCE_DIR}/cmake/Logging.cmake)

    find_program(DOXYGEN_EXECUTABLE doxygen REQUIRED)

    add_custom_target(docs ALL
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Generating documentation with Doxygen"
        VERBATIM
    )

    pir_log_header("Documentation Build")
    pir_log_kv("Doxygen" "${DOXYGEN_EXECUTABLE}")
    pir_log_kv("Output" "docs/html/")
    pir_log_footer()

    return()
endif()

# =============================================================================
# Compilation build
# =============================================================================
include(${CMAKE_SOURCE_DIR}/cmake/Toolchain.cmake)
project(agent LANGUAGES CXX)
include(${CMAKE_SOURCE_DIR}/cmake/Common.cmake)

# =============================================================================
# Build metadata
# =============================================================================
# BUILD_NUMBER / COMMIT_HASH stay CACHE variables: CI injects real values.
# API_VERSION / NAME_ID are normal variables: they are protocol constants that
# must track the wire structs in this source tree, so a stale CMakeCache.txt in
# an old build tree (or an external -D override) must not pin a wrong value —
# an X-Api-Version header reporting a mismatched version silently
# desyncs the C2 parser.
set(BUILD_NUMBER "0" CACHE STRING "Agent build number")
set(COMMIT_HASH "00000000" CACHE STRING "Agent commit hash")
set(API_VERSION "1")
set(NAME_ID "0")
list(APPEND PIR_DEFINES
    AGENT_BUILD_NUMBER=${BUILD_NUMBER}
    AGENT_COMMIT_HASH="${COMMIT_HASH}"
    AGENT_API_VERSION=${API_VERSION}
    AGENT_NAME_ID=${NAME_ID}
)

include(${PIR_ROOT_DIR}/cmake/Target.cmake)
