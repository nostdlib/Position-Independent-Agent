# =============================================================================
# Common.cmake - Build System Orchestrator
# =============================================================================
# Includes sub-modules in dependency order. Each module handles a single
# responsibility: option validation, triple mapping, flag assembly, source
# collection, and post-build artifact generation.

include_guard(GLOBAL)

# Auto-discover PIR root from this file's location (cmake/Common.cmake)
get_filename_component(PIR_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

include(${PIR_ROOT_DIR}/cmake/Logging.cmake)
include(${PIR_ROOT_DIR}/cmake/Options.cmake)
include(${PIR_ROOT_DIR}/cmake/Triples.cmake)
include(${PIR_ROOT_DIR}/cmake/CompilerFlags.cmake)
include(${PIR_ROOT_DIR}/cmake/Sources.cmake)
include(${PIR_ROOT_DIR}/cmake/PICTransform.cmake)
include(${PIR_ROOT_DIR}/cmake/PolyTransform.cmake)
include(${PIR_ROOT_DIR}/cmake/PostBuild.cmake)
