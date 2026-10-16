cmake_minimum_required(VERSION 3.24)
if(WIN32)
 project(FlareLensAE LANGUAGES C CXX RC)
else()
 project(FlareLensAE LANGUAGES C CXX)
endif()
set(CMAKE_CXX_STANDARD 20)
# CUDA static runtime needs the same CRT as the native AE plugin objects.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Never embed one developer's drive letters in the public source tree.
# Existing CMakeCache values remain valid; fresh clones use environment vars.
set(AE_SDK_ROOT "$ENV{AE_SDK_ROOT}" CACHE PATH "Adobe AE SDK Examples directory")
add_library(FlareLensCore STATIC upstream/src/lens.cpp upstream/src/trace.cpp
            upstream/src/ghost.cpp upstream/src/starburst.cpp src/FlareLensCore.cpp
            src/FlareLensOptics.cpp src/FlareLensCpuParallel.cpp)
target_include_directories(FlareLensCore PUBLIC upstream/src src)
target_compile_definitions(FlareLensCore PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
 target_compile_options(FlareLensCore PRIVATE /utf-8)
endif()
add_executable(FlareLensCoreTest tests/core_test.cpp)
target_link_libraries(FlareLensCoreTest PRIVATE FlareLensCore)
enable_testing()
add_test(NAME FlareLensCoreTest COMMAND FlareLensCoreTest)
add_executable(FlareLensPupilCacheTest tests/pupil_cache_test.cpp)
target_link_libraries(FlareLensPupilCacheTest PRIVATE FlareLensCore)
add_test(NAME FlareLensPupilCacheTest COMMAND FlareLensPupilCacheTest)
add_executable(FlareLensHostGeometryTest tests/host_geometry_test.cpp)
target_include_directories(FlareLensHostGeometryTest PRIVATE src)
add_test(NAME FlareLensHostGeometryTest COMMAND FlareLensHostGeometryTest)
add_executable(FlareLensCompositeTest tests/composite_test.cpp)
target_link_libraries(FlareLensCompositeTest PRIVATE FlareLensCore)
add_test(NAME FlareLensCompositeTest COMMAND FlareLensCompositeTest)
add_executable(FlareLensVisibilityTest tests/visibility_test.cpp)
target_link_libraries(FlareLensVisibilityTest PRIVATE FlareLensCore)
add_test(NAME FlareLensVisibilityTest COMMAND FlareLensVisibilityTest)
add_executable(FlareLensOpticsTest tests/optics_test.cpp)
target_link_libraries(FlareLensOpticsTest PRIVATE FlareLensCore)
add_test(NAME FlareLensOpticsTest COMMAND FlareLensOpticsTest)
add_executable(FlareLensCatalogTest tests/lens_catalog_test.cpp)
target_link_libraries(FlareLensCatalogTest PRIVATE FlareLensCore)
add_test(NAME FlareLensCatalogTest COMMAND FlareLensCatalogTest)
add_executable(FlareLensCpuParallelTest tests/cpu_parallel_test.cpp)
target_link_libraries(FlareLensCpuParallelTest PRIVATE FlareLensCore)
add_test(NAME FlareLensCpuParallelTest COMMAND FlareLensCpuParallelTest)
# Embed the EXACT upstream Nuke catalog in the AEX. No .lens deployment
# required for rendering; presets never silently fall back to Nikkor 50mm.
set(FLARE_LENS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/upstream/lenses/lens_files")
# Some upstream filenames contain UNPAIRED '[' characters. CMake's list
# evaluator treats ';' separators after those brackets as one item,
# collapsing 1370 real names to 261 entries. Escape square brackets
# before calling ANY CMake list operations; restore only inside the loop.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/src/NukeLensCatalogOriginal.txt"
     FLARE_LENS_NAMES)
string(REPLACE "[" "__FLARE_CMAKE_LBRACKET__" FLARE_LENS_NAMES "${FLARE_LENS_NAMES}")
string(REPLACE "]" "__FLARE_CMAKE_RBRACKET__" FLARE_LENS_NAMES "${FLARE_LENS_NAMES}")
list(LENGTH FLARE_LENS_NAMES FLARE_LENS_COUNT)
if(NOT FLARE_LENS_COUNT EQUAL 1370)
 message(FATAL_ERROR "Expected exactly 1370 original Nuke lenses, got ${FLARE_LENS_COUNT}")
endif()
if(FLARE_LENS_COUNT LESS 1 OR FLARE_LENS_COUNT GREATER 12767)
 message(FATAL_ERROR "Invalid Nuke lens catalog size: ${FLARE_LENS_COUNT}")
endif()
set(FLARE_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${FLARE_GEN_DIR}")
set(FLARE_LENS_HEADER "#pragma once\n#include <array>\nnamespace fl::catalog {\ninline constexpr int kCount=${FLARE_LENS_COUNT};\ninline constexpr int kResourceBase=20000;\ninline constexpr std::array<const char*,kCount> kNames={{\n")
set(FLARE_LENS_RC "// Original Nuke lens prescriptions, embedded by immutable index.\n")
set(FLARE_LENS_INDEX "")
set(FLARE_LENS_I 0)
foreach(FLARE_LENS_NAME_ESC IN LISTS FLARE_LENS_NAMES)
 string(REPLACE "__FLARE_CMAKE_LBRACKET__" "[" FLARE_LENS_NAME "${FLARE_LENS_NAME_ESC}")
 string(REPLACE "__FLARE_CMAKE_RBRACKET__" "]" FLARE_LENS_NAME "${FLARE_LENS_NAME}")
 set(FLARE_LENS_FILE "${FLARE_LENS_ROOT}/${FLARE_LENS_NAME}")
 if(NOT EXISTS "${FLARE_LENS_FILE}")
  message(FATAL_ERROR "Missing required original Nuke lens: ${FLARE_LENS_NAME}")
 endif()
 string(REPLACE "\\" "\\\\" FLARE_ESCAPED "${FLARE_LENS_NAME}")
 string(REPLACE "\"" "\\\"" FLARE_ESCAPED "${FLARE_ESCAPED}")
 string(APPEND FLARE_LENS_HEADER " \"${FLARE_ESCAPED}\",\n")
 math(EXPR FLARE_LENS_RESOURCE "20000+${FLARE_LENS_I}")
 file(TO_CMAKE_PATH "${FLARE_LENS_FILE}" FLARE_RESOURCE_PATH)
 string(APPEND FLARE_LENS_RC "${FLARE_LENS_RESOURCE} RCDATA \"${FLARE_RESOURCE_PATH}\"\n")
 math(EXPR FLARE_USER_ID "${FLARE_LENS_I}+1")
 string(APPEND FLARE_LENS_INDEX "${FLARE_USER_ID}\t${FLARE_LENS_NAME}\n")
 math(EXPR FLARE_LENS_I "${FLARE_LENS_I}+1")
endforeach()
string(APPEND FLARE_LENS_HEADER "}};\n}\n")
file(WRITE "${FLARE_GEN_DIR}/FlareLensCatalogGenerated.hpp" "${FLARE_LENS_HEADER}")
file(WRITE "${FLARE_GEN_DIR}/FlareLensCatalogEmbedded.rc" "${FLARE_LENS_RC}")
file(WRITE "${FLARE_GEN_DIR}/FlareLensCatalog.tsv" "${FLARE_LENS_INDEX}")
message(STATUS "Embedding ${FLARE_LENS_COUNT} original Nuke lens prescriptions")

# Cross-platform binary catalog parity: the macOS embedded byte table must
# match each original Windows RCDATA .lens file byte-for-byte.
if(APPLE)
 find_package(Python3 COMPONENTS Interpreter REQUIRED)
else()
 # Python is optional for normal Windows builds; it only enables an extra
 # cross-platform lens-catalog integrity test.
 find_package(Python3 COMPONENTS Interpreter QUIET)
endif()
if(Python3_Interpreter_FOUND)
set(FLARE_MAC_EMBED_CPP "${FLARE_GEN_DIR}/FlareLensEmbeddedMac.cpp")
add_custom_command(OUTPUT "${FLARE_MAC_EMBED_CPP}"
 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_lenses_mac.py"
 --manifest "${CMAKE_CURRENT_SOURCE_DIR}/src/NukeLensCatalogOriginal.txt"
 --root "${FLARE_LENS_ROOT}" --output "${FLARE_MAC_EMBED_CPP}"
 DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_lenses_mac.py"
   "${CMAKE_CURRENT_SOURCE_DIR}/src/NukeLensCatalogOriginal.txt"
 COMMENT "Embedding original lens bytes for cross-platform verification"
 VERBATIM)
add_executable(FlareLensMacCatalogEmbeddingTest
 tests/mac_catalog_embedding_test.cpp "${FLARE_MAC_EMBED_CPP}")
target_include_directories(FlareLensMacCatalogEmbeddingTest PRIVATE
 src "${FLARE_GEN_DIR}")
target_compile_definitions(FlareLensMacCatalogEmbeddingTest PRIVATE
 FLARELENS_TEST_SOURCE_DIR="${FLARE_LENS_ROOT}")
add_test(NAME FlareLensMacCatalogEmbeddingTest
 COMMAND FlareLensMacCatalogEmbeddingTest)
endif()

option(FLARELENS_BUILD_AE "Build AE effect" ON)
option(FLARELENS_ENABLE_CUDA "Native AE host-managed CUDA render" ON)
if(APPLE)
 if(FLARELENS_BUILD_AE)
  include(cmake/MacOS.cmake)
 endif()
else()
if(FLARELENS_BUILD_AE)
 if(NOT EXISTS "${AE_SDK_ROOT}/Headers/AE_Effect.h")
  message(FATAL_ERROR "Set -DAE_SDK_ROOT=<Adobe After Effects SDK Examples directory>")
 endif()
 # Native AE SDK adapter: no dependency on a private sibling project.
 # Mirrors the minimal Adobe headers / SuiteHelper usage required by the effect.
 if(NOT EXISTS "${AE_SDK_ROOT}/Util/AEFX_SuiteHelper.c")
  message(FATAL_ERROR "Adobe SDK is missing Util/AEFX_SuiteHelper.c")
 endif()
 add_library(FlareLensAESDK INTERFACE)
 target_include_directories(FlareLensAESDK INTERFACE
  "${AE_SDK_ROOT}/Headers" "${AE_SDK_ROOT}/Headers/SP"
  "${AE_SDK_ROOT}/Headers/Win" "${AE_SDK_ROOT}/Util")
 target_compile_definitions(FlareLensAESDK INTERFACE MSWindows WIN32 _WINDOWS)
 target_sources(FlareLensAESDK INTERFACE "${AE_SDK_ROOT}/Util/AEFX_SuiteHelper.c")
 add_library(FlareLensAE SHARED
   src/FlareLensEffect.cpp src/FlareLensCatalog.cpp
   src/FlareLensCustomUI.cpp src/FlareLensCatalogPicker.cpp
   src/FlareLensPiPL.rc src/FlareLensCatalogPicker.rc
   "${FLARE_GEN_DIR}/FlareLensCatalogEmbedded.rc")
 target_include_directories(FlareLensAE PRIVATE "${FLARE_GEN_DIR}")
 target_link_libraries(FlareLensAE PRIVATE comdlg32)
 target_link_libraries(FlareLensAE PRIVATE FlareLensCore FlareLensAESDK)
 target_compile_options(FlareLensAE PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
 set_target_properties(FlareLensAE PROPERTIES PREFIX "" SUFFIX ".aex")
 # All 1,370 original Nuke lens files are compiled directly into the AEX
 # as read-only RCDATA resources. No sidecar .lens files need installation.
 # Stage only human-readable manifest and required attribution, not 1,370
 # redundant post-build file copies.
 add_custom_command(TARGET FlareLensAE POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
   "${CMAKE_CURRENT_SOURCE_DIR}/upstream/LICENSE"
   "$<TARGET_FILE_DIR:FlareLensAE>/LICENSE.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
   "${FLARE_GEN_DIR}/FlareLensCatalog.tsv"
   "$<TARGET_FILE_DIR:FlareLensAE>/FlareLensCatalog.tsv"
  VERBATIM)
 add_executable(FlareLensFullCatalogTest tests/full_catalog_test.cpp)
 target_link_libraries(FlareLensFullCatalogTest PRIVATE FlareLensCore)
 target_compile_options(FlareLensFullCatalogTest PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
 target_include_directories(FlareLensFullCatalogTest PRIVATE
   src "${FLARE_GEN_DIR}")
 add_dependencies(FlareLensFullCatalogTest FlareLensAE)
 add_test(NAME FlareLensFullCatalogTest COMMAND FlareLensFullCatalogTest)
 add_executable(FlareLensEntryTest tests/entry_test.cpp)
 target_include_directories(FlareLensEntryTest PRIVATE "${AE_SDK_ROOT}/Headers")
 add_dependencies(FlareLensEntryTest FlareLensAE)
 add_test(NAME FlareLensEntryTest COMMAND FlareLensEntryTest)
 add_executable(FlareLensParamsTest tests/params_test.cpp)
 target_include_directories(FlareLensParamsTest PRIVATE
   src upstream/src "${FLARE_GEN_DIR}" "${AE_SDK_ROOT}/Headers")
 target_compile_options(FlareLensParamsTest PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
 add_dependencies(FlareLensParamsTest FlareLensAE)
 add_test(NAME FlareLensParamsTest COMMAND FlareLensParamsTest)
 add_executable(FlareLensCustomUIEventTest tests/custom_ui_event_test.cpp)
 target_include_directories(FlareLensCustomUIEventTest PRIVATE
   src upstream/src "${AE_SDK_ROOT}/Headers" "${AE_SDK_ROOT}/Headers/SP")
 target_compile_options(FlareLensCustomUIEventTest PRIVATE
   $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
 add_dependencies(FlareLensCustomUIEventTest FlareLensAE)
 add_test(NAME FlareLensCustomUIEventTest COMMAND FlareLensCustomUIEventTest)
 add_executable(FlareLensCustomKeyframeTest tests/custom_keyframe_test.cpp)
 target_include_directories(FlareLensCustomKeyframeTest PRIVATE
   src upstream/src "${AE_SDK_ROOT}/Headers" "${AE_SDK_ROOT}/Headers/SP")
 target_compile_options(FlareLensCustomKeyframeTest PRIVATE
   $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
 add_dependencies(FlareLensCustomKeyframeTest FlareLensAE)
 add_test(NAME FlareLensCustomKeyframeTest COMMAND FlareLensCustomKeyframeTest)
 add_executable(FlareLensCustomUIDrawTest tests/custom_ui_draw_test.cpp)
 target_include_directories(FlareLensCustomUIDrawTest PRIVATE
   src upstream/src "${AE_SDK_ROOT}/Headers" "${AE_SDK_ROOT}/Headers/SP")
 target_compile_options(FlareLensCustomUIDrawTest PRIVATE
   $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
 add_dependencies(FlareLensCustomUIDrawTest FlareLensAE)
 add_test(NAME FlareLensCustomUIDrawTest COMMAND FlareLensCustomUIDrawTest)
 # Call the actual AEX entry point with a host-shaped, fully transparent
 # RGBA image and a NULL SmartFX input, without launching After Effects.
 add_executable(FlareLensHostRenderTest tests/host_render_test.cpp)
 target_include_directories(FlareLensHostRenderTest PRIVATE
   src upstream/src "${AE_SDK_ROOT}/Headers" "${AE_SDK_ROOT}/Headers/SP")
 add_dependencies(FlareLensHostRenderTest FlareLensAE)
 set_target_properties(FlareLensHostRenderTest PROPERTIES OUTPUT_NAME FlareLensHostRenderTest_v138)
 add_test(NAME FlareLensHostRenderTest COMMAND FlareLensHostRenderTest)
 add_executable(FlareLensHdrDepthTest tests/hdr_depth_test.cpp)
 target_include_directories(FlareLensHdrDepthTest PRIVATE
   "${AE_SDK_ROOT}/Headers" "${AE_SDK_ROOT}/Headers/SP")
 add_dependencies(FlareLensHdrDepthTest FlareLensAE)
 add_test(NAME FlareLensHdrDepthTest COMMAND FlareLensHdrDepthTest)
 # Real AEX factory settings must be visible without artificially turning
 # its Manual Source Radiance into 10,000+, and must match a single Nuke HDR
 # brightness-block source exactly when manual diameter is set to zero.
 add_executable(FlareLensManualSourceTest tests/manual_source_test.cpp)
 target_include_directories(FlareLensManualSourceTest PRIVATE
   "${AE_SDK_ROOT}/Headers" "${AE_SDK_ROOT}/Headers/SP" src)
 add_dependencies(FlareLensManualSourceTest FlareLensAE)
 set_target_properties(FlareLensManualSourceTest PROPERTIES OUTPUT_NAME FlareLensManualSourceTest_v138)
 add_test(NAME FlareLensManualSourceTest COMMAND FlareLensManualSourceTest)
 if(FLARELENS_ENABLE_CUDA)
  include(CheckLanguage)
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
   enable_language(CUDA)
   find_package(CUDAToolkit REQUIRED)
   target_sources(FlareLensAE PRIVATE src/FlareLensGpu.cu src/FlareLensGpuDetection.cu)
   target_compile_definitions(FlareLensAE PRIVATE FLARELENS_HAVE_CUDA=1)
   target_compile_definitions(FlareLensEntryTest PRIVATE FLARELENS_HAVE_CUDA=1)
   target_compile_definitions(FlareLensHostRenderTest PRIVATE FLARELENS_HAVE_CUDA=1)
   target_link_libraries(FlareLensAE PRIVATE CUDA::cudart_static)
   set_target_properties(FlareLensAE PROPERTIES CUDA_STANDARD 17
     CUDA_ARCHITECTURES "75;86;89")
   add_executable(FlareLensGpuDetectionTest
     tests/gpu_detection_test.cpp src/FlareLensGpuDetection.cu)
   target_link_libraries(FlareLensGpuDetectionTest PRIVATE FlareLensCore CUDA::cudart_static)
   target_compile_options(FlareLensGpuDetectionTest PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
   set_target_properties(FlareLensGpuDetectionTest PROPERTIES
     CUDA_STANDARD 17 CUDA_ARCHITECTURES "75;86;89")
   add_test(NAME FlareLensGpuDetectionTest COMMAND FlareLensGpuDetectionTest)
   set_tests_properties(FlareLensGpuDetectionTest PROPERTIES SKIP_RETURN_CODE 77)
   add_executable(FlareLensGpuTest tests/gpu_test.cpp src/FlareLensGpu.cu)
   target_link_libraries(FlareLensGpuTest PRIVATE FlareLensCore CUDA::cudart_static)
   target_compile_options(FlareLensGpuTest PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
   set_target_properties(FlareLensGpuTest PROPERTIES CUDA_STANDARD 17 CUDA_ARCHITECTURES "75;86;89")
   add_test(NAME FlareLensGpuTest COMMAND FlareLensGpuTest)
   set_tests_properties(FlareLensGpuTest PROPERTIES SKIP_RETURN_CODE 77)
   # Required: Nuke-equivalent GPU blur/haze, transparent AE RGBA, and the
   # user's actual 1920x1280 / 128 rays / 7 bands optical configuration.
   add_executable(FlareLensGpuPostTest tests/gpu_post_test.cpp src/FlareLensGpu.cu)
   target_link_libraries(FlareLensGpuPostTest PRIVATE FlareLensCore CUDA::cudart_static)
   target_compile_options(FlareLensGpuPostTest PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
   set_target_properties(FlareLensGpuPostTest PROPERTIES
      CUDA_STANDARD 17 CUDA_ARCHITECTURES "75;86;89")
   add_test(NAME FlareLensGpuPostTest COMMAND FlareLensGpuPostTest)
   set_tests_properties(FlareLensGpuPostTest PROPERTIES SKIP_RETURN_CODE 77)
   add_executable(FlareLensGpuStreamTest tests/gpu_stream_test.cpp src/FlareLensGpu.cu)
   target_link_libraries(FlareLensGpuStreamTest PRIVATE FlareLensCore CUDA::cudart_static)
   target_compile_options(FlareLensGpuStreamTest PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
   set_target_properties(FlareLensGpuStreamTest PROPERTIES
     CUDA_STANDARD 17 CUDA_ARCHITECTURES "75;86;89")
   add_test(NAME FlareLensGpuStreamTest COMMAND FlareLensGpuStreamTest)
   set_tests_properties(FlareLensGpuStreamTest PROPERTIES SKIP_RETURN_CODE 77)
   # Loads the actual built .aex and executes the production CUDA SmartFX
   # selector through a realistic Adobe SDK suite mock. No AE GUI or process.
   add_executable(FlareLensHostGpuTest tests/host_gpu_test.cpp)
   target_include_directories(FlareLensHostGpuTest PRIVATE "${FLARE_GEN_DIR}"
     "${AE_SDK_ROOT}/Headers" "${AE_SDK_ROOT}/Headers/SP")
   target_link_libraries(FlareLensHostGpuTest PRIVATE FlareLensCore CUDA::cudart_static)
   target_compile_options(FlareLensHostGpuTest PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
   add_dependencies(FlareLensHostGpuTest FlareLensAE)
   add_test(NAME FlareLensHostGpuTest COMMAND FlareLensHostGpuTest)
   set_tests_properties(FlareLensHostGpuTest PROPERTIES SKIP_RETURN_CODE 77)
   # True upstream reference: separate executable compiles the unmodified
   # Nuke ghost_cuda.cu. Never link both CUDA launchers into one binary.
   add_executable(FlareLensUpstreamReference
     tests/upstream_reference.cpp upstream/src/ghost_cuda.cu)
   target_link_libraries(FlareLensUpstreamReference PRIVATE
     FlareLensCore CUDA::cudart_static)
   target_compile_options(FlareLensUpstreamReference PRIVATE
     $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
   set_target_properties(FlareLensUpstreamReference PROPERTIES
     CUDA_STANDARD 17 CUDA_ARCHITECTURES "75;86;89")
   add_test(NAME FlareLensUpstreamReference COMMAND FlareLensUpstreamReference)
   set_tests_properties(FlareLensUpstreamReference PROPERTIES
     SKIP_RETURN_CODE 77 FIXTURES_SETUP NukeReference)
   add_executable(FlareLensUpstreamParity
     tests/upstream_parity.cpp src/FlareLensGpu.cu)
   target_link_libraries(FlareLensUpstreamParity PRIVATE
     FlareLensCore CUDA::cudart_static)
   target_compile_options(FlareLensUpstreamParity PRIVATE
     $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
   set_target_properties(FlareLensUpstreamParity PROPERTIES
     CUDA_STANDARD 17 CUDA_ARCHITECTURES "75;86;89")
   add_test(NAME FlareLensUpstreamParity COMMAND FlareLensUpstreamParity)
   set_tests_properties(FlareLensUpstreamParity PROPERTIES
     SKIP_RETURN_CODE 77 FIXTURES_REQUIRED NukeReference)
   add_executable(FlareLensBenchmark tests/bench.cpp src/FlareLensGpu.cu)
   target_link_libraries(FlareLensBenchmark PRIVATE FlareLensCore CUDA::cudart_static)
   target_compile_options(FlareLensBenchmark PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
   set_target_properties(FlareLensBenchmark PROPERTIES CUDA_STANDARD 17 CUDA_ARCHITECTURES "75;86;89")
  else()
   message(WARNING "CUDA compiler not found; compiling CPU-only SmartFX plugin.")
  endif()
 endif()
endif()
# End Windows-only AE integration; macOS uses cmake/MacOS.cmake.
endif()
