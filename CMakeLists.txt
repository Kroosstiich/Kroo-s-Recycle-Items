cmake_minimum_required(VERSION 3.25)
project(KroosRecycleItems VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(KRI_COMMONLIB_DIR "" CACHE PATH "CommonLibSSE-NG 9.1.0 source tree")
if(NOT EXISTS "${KRI_COMMONLIB_DIR}/CMakeLists.txt")
	message(FATAL_ERROR "KRI_COMMONLIB_DIR does not point at CommonLibSSE-NG: '${KRI_COMMONLIB_DIR}'")
endif()

set(ENABLE_SKYRIM_SE ON CACHE BOOL " " FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL " " FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL " " FORCE)
set(BUILD_TESTS OFF CACHE BOOL " " FORCE)

add_subdirectory("${KRI_COMMONLIB_DIR}" CommonLibSSE EXCLUDE_FROM_ALL)
get_target_property(COMMONLIB_SRC_DIR CommonLibSSE SOURCE_DIR)
include("${COMMONLIB_SRC_DIR}/cmake/CommonLibSSE.cmake")

add_commonlibsse_plugin(${PROJECT_NAME}
	AUTHOR "Kroosstii"
	SOURCES
		src/main.cpp
		src/RecipeScan.cpp
		src/RecycleMenu.cpp
		src/Recycler.cpp
		src/Settings.cpp
		src/Text.cpp
)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_compile_definitions(${PROJECT_NAME} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_precompile_headers(${PROJECT_NAME} PRIVATE include/PCH.h)
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src")
