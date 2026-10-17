# Native macOS AE plugin with Metal optical renderer.
# Proprietary Adobe SDK input must be supplied outside repository.
enable_language(OBJCXX)
find_library(FLARE_COCOA Cocoa REQUIRED)
find_library(FLARE_METAL Metal REQUIRED)
find_library(FLARE_FOUNDATION Foundation REQUIRED)
find_program(FLARE_XCRUN xcrun REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(FLARE_METAL_PHYSICS "${FLARE_GEN_DIR}/FlareLensMetalPhysics.metal")
set(FLARE_METAL_AIR "${FLARE_GEN_DIR}/FlareLensMetal.air")
set(FLARE_METAL_LIBRARY "${FLARE_GEN_DIR}/FlareLensMetal.metallib")
add_custom_command(OUTPUT "${FLARE_METAL_PHYSICS}"
 COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_metal_physics.py"
  --cuda "${CMAKE_CURRENT_SOURCE_DIR}/src/FlareLensGpu.cu"
  --output "${FLARE_METAL_PHYSICS}"
 DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_metal_physics.py"
         "${CMAKE_CURRENT_SOURCE_DIR}/src/FlareLensGpu.cu"
 VERBATIM)
add_custom_command(OUTPUT "${FLARE_METAL_AIR}"
 COMMAND "${FLARE_XCRUN}" -sdk macosx metal
  -I "${FLARE_GEN_DIR}" -c
  "${CMAKE_CURRENT_SOURCE_DIR}/src/FlareLensMetalKernels.metal"
  -o "${FLARE_METAL_AIR}"
 DEPENDS "${FLARE_METAL_PHYSICS}"
         "${CMAKE_CURRENT_SOURCE_DIR}/src/FlareLensMetalKernels.metal"
 VERBATIM)
add_custom_command(OUTPUT "${FLARE_METAL_LIBRARY}"
 COMMAND "${FLARE_XCRUN}" -sdk macosx metallib
  "${FLARE_METAL_AIR}" -o "${FLARE_METAL_LIBRARY}"
 DEPENDS "${FLARE_METAL_AIR}"
 VERBATIM)
add_custom_target(FlareLensMetalShaders DEPENDS "${FLARE_METAL_LIBRARY}")
if(NOT EXISTS "${AE_SDK_ROOT}/Headers/AE_Effect.h" OR
   NOT EXISTS "${AE_SDK_ROOT}/Util/AEFX_SuiteHelper.c")
 message(FATAL_ERROR "macOS AE SDK missing; set AE_SDK_ROOT to its Examples directory")
endif()
add_library(FlareLensAE MODULE
 src/FlareLensEffect.cpp src/FlareLensCatalog.cpp
 src/FlareLensCustomUI.cpp src/FlareLensCatalogPickerMac.mm
 src/FlareLensMetal.mm
 "${FLARE_MAC_EMBED_CPP}" "${AE_SDK_ROOT}/Util/AEFX_SuiteHelper.c")
target_include_directories(FlareLensAE PRIVATE
 "${FLARE_GEN_DIR}" src upstream/src
 "${AE_SDK_ROOT}/Headers" "${AE_SDK_ROOT}/Headers/SP"
 "${AE_SDK_ROOT}/Headers/Mac" "${AE_SDK_ROOT}/Util")
target_link_libraries(FlareLensAE PRIVATE FlareLensCore
 "${FLARE_COCOA}" "${FLARE_METAL}" "${FLARE_FOUNDATION}")
target_compile_definitions(FlareLensAE PRIVATE FLARELENS_HAVE_METAL=1)
add_dependencies(FlareLensAE FlareLensMetalShaders)
set_target_properties(FlareLensAE PROPERTIES
 PREFIX "" OUTPUT_NAME "FlareSimAE"
 BUNDLE TRUE BUNDLE_EXTENSION "plugin"
 CXX_VISIBILITY_PRESET default)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FlareSimAE.Info.plist.in"
 "${FLARE_GEN_DIR}/FlareSimAE.Info.plist" @ONLY)
file(WRITE "${FLARE_GEN_DIR}/FlareSimAE.PkgInfo" "eFKTFXTC")
set_property(TARGET FlareLensAE APPEND PROPERTY LINK_DEPENDS
 "${CMAKE_CURRENT_SOURCE_DIR}/src/FlareLensPiPL.r"
 "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FlareSimAE.Info.plist.in")
target_compile_options(FlareLensAE PRIVATE
 $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
 $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
add_custom_command(TARGET FlareLensAE POST_BUILD
 COMMAND "${CMAKE_COMMAND}" -E copy_if_different
  "${FLARE_GEN_DIR}/FlareSimAE.Info.plist"
  "$<TARGET_BUNDLE_CONTENT_DIR:FlareLensAE>/Info.plist"
 COMMAND "${CMAKE_COMMAND}" -E copy_if_different
  "${FLARE_GEN_DIR}/FlareSimAE.PkgInfo"
  "$<TARGET_BUNDLE_CONTENT_DIR:FlareLensAE>/PkgInfo"
 COMMAND "${CMAKE_COMMAND}" -E make_directory
  "$<TARGET_BUNDLE_CONTENT_DIR:FlareLensAE>/Resources"
 COMMAND "${CMAKE_COMMAND}" -E copy_if_different
  "${FLARE_METAL_LIBRARY}"
  "$<TARGET_BUNDLE_CONTENT_DIR:FlareLensAE>/Resources/FlareLensMetal.metallib"
 COMMAND "${FLARE_XCRUN}" Rez -useDF
  -i "${AE_SDK_ROOT}/Headers"
  -i "${AE_SDK_ROOT}/Resources"
  -d AE_OS_MAC
  -o "$<TARGET_BUNDLE_CONTENT_DIR:FlareLensAE>/Resources/FlareSimAE.rsrc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/FlareLensPiPL.r"
 VERBATIM COMMENT "Building macOS PiPL resource using Apple's Rez")

# Hardware-backed Metal shader and optical parity test. Run on macOS CI;
# universal compilation alone cannot prove the kernels actually execute.
add_executable(FlareLensMetalGpuTest
 tests/metal_gpu_test.mm src/FlareLensMetal.mm)
target_include_directories(FlareLensMetalGpuTest PRIVATE
 src upstream/src "${FLARE_GEN_DIR}")
target_link_libraries(FlareLensMetalGpuTest PRIVATE
 FlareLensCore "${FLARE_METAL}" "${FLARE_FOUNDATION}")
target_compile_options(FlareLensMetalGpuTest PRIVATE
 $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_compile_definitions(FlareLensMetalGpuTest PRIVATE
 FLARELENS_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_dependencies(FlareLensMetalGpuTest FlareLensMetalShaders)
add_test(NAME FlareLensMetalGpuTest COMMAND FlareLensMetalGpuTest)
set_tests_properties(FlareLensMetalGpuTest PROPERTIES
 ENVIRONMENT "FLARELENS_METAL_LIBRARY=${FLARE_METAL_LIBRARY}"
 SKIP_RETURN_CODE 77)
