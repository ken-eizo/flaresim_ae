# Native macOS AE plugin (CPU optical renderer).
# Proprietary Adobe SDK input must be supplied outside repository.
enable_language(OBJCXX)
find_library(FLARE_COCOA Cocoa REQUIRED)
if(NOT EXISTS "${AE_SDK_ROOT}/Headers/AE_Effect.h" OR
   NOT EXISTS "${AE_SDK_ROOT}/Util/AEFX_SuiteHelper.c")
 message(FATAL_ERROR "macOS AE SDK missing; set AE_SDK_ROOT to its Examples directory")
endif()
add_library(FlareLensAE MODULE
 src/FlareLensEffect.cpp src/FlareLensCatalog.cpp
 src/FlareLensCustomUI.cpp src/FlareLensCatalogPickerMac.mm
 "${FLARE_MAC_EMBED_CPP}" "${AE_SDK_ROOT}/Util/AEFX_SuiteHelper.c")
target_include_directories(FlareLensAE PRIVATE
 "${FLARE_GEN_DIR}" src upstream/src
 "${AE_SDK_ROOT}/Headers" "${AE_SDK_ROOT}/Headers/SP"
 "${AE_SDK_ROOT}/Headers/Mac" "${AE_SDK_ROOT}/Util")
target_link_libraries(FlareLensAE PRIVATE FlareLensCore "${FLARE_COCOA}")
set_target_properties(FlareLensAE PROPERTIES
 PREFIX "" OUTPUT_NAME "FlareSimAE"
 BUNDLE TRUE BUNDLE_EXTENSION "plugin"
 CXX_VISIBILITY_PRESET default)
target_compile_options(FlareLensAE PRIVATE
 $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
 $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
find_program(FLARE_XCRUN xcrun REQUIRED)
add_custom_command(TARGET FlareLensAE POST_BUILD
 COMMAND "${CMAKE_COMMAND}" -E make_directory
  "$<TARGET_BUNDLE_CONTENT_DIR:FlareLensAE>/Resources"
 COMMAND "${FLARE_XCRUN}" Rez
  -i "${AE_SDK_ROOT}/Headers"
  -i "${AE_SDK_ROOT}/Resources"
  -d AE_OS_MAC
  -o "$<TARGET_BUNDLE_CONTENT_DIR:FlareLensAE>/Resources/FlareSimAE.rsrc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/FlareLensPiPL.r"
 VERBATIM COMMENT "Building macOS PiPL resource using Apple's Rez")
