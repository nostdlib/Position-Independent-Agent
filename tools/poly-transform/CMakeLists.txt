cmake_minimum_required(VERSION 3.20)
project(PolyTransform LANGUAGES CXX)

# ── Static-link C++ runtime for portable binaries ────────────────────────────
option(STATIC_LINK "Statically link C++ runtime" OFF)
if(STATIC_LINK AND MSVC)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(poly-transform main.cpp)
target_compile_features(poly-transform PRIVATE cxx_std_17)

if(STATIC_LINK)
	message(STATUS "[poly-transform] Static linking enabled")
	if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
		target_link_options(poly-transform PRIVATE -static-libgcc -static-libstdc++)
	elseif(APPLE)
		target_link_options(poly-transform PRIVATE -static-libstdc++)
	endif()
endif()
