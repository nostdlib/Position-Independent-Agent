# =============================================================================
# agent - Built on Position-Independent Runtime
# =============================================================================

cmake_minimum_required(VERSION 3.20)
include(${CMAKE_SOURCE_DIR}/runtime/cmake/Toolchain.cmake)
project(agent LANGUAGES CXX)
include(${CMAKE_SOURCE_DIR}/runtime/cmake/Common.cmake)

# =============================================================================
# Build metadata
# =============================================================================
set(BUILD_NUMBER "0" CACHE STRING "Agent build number")
set(COMMIT_HASH "00000000" CACHE STRING "Agent commit hash")
list(APPEND PIR_DEFINES
    AGENT_BUILD_NUMBER=${BUILD_NUMBER}
    AGENT_COMMIT_HASH="${COMMIT_HASH}"
)

include(${PIR_ROOT_DIR}/cmake/Target.cmake)
