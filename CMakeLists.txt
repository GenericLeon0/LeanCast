cmake_minimum_required(VERSION 3.22)

if(NOT WIN32)
  message(FATAL_ERROR "LeanCast Native is a Windows-only Win32 application.")
endif()

project(LeanCastNative VERSION 0.4.5 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(LEANCAST_ICON "${CMAKE_CURRENT_SOURCE_DIR}/build/icon.ico")
set(LEANCAST_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/native/app.manifest")
configure_file(native/LeanCast.rc.in native/LeanCast.rc @ONLY)
configure_file(native/src/version.hpp.in generated/version.hpp @ONLY)

add_library(LeanCastSQLite STATIC
  third_party/sqlite/sqlite3.c
)

target_include_directories(LeanCastSQLite PUBLIC third_party/sqlite)

target_compile_definitions(LeanCastSQLite PRIVATE
  SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_THREADSAFE=1
)

add_executable(LeanCast WIN32
  native/src/main.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/native/LeanCast.rc
)

add_executable(LeanCastPluginHost
  native/src/plugin_host.cpp
)

target_compile_definitions(LeanCast PRIVATE
  UNICODE
  _UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
)

target_include_directories(LeanCast PRIVATE native/src "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_compile_definitions(LeanCastPluginHost PRIVATE
  UNICODE
  _UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
)

target_include_directories(LeanCastPluginHost PRIVATE native/src)

target_link_libraries(LeanCast PRIVATE
  LeanCastSQLite
  d2d1
  dwrite
  windowscodecs
  shell32
  shlwapi
  ole32
  oleaut32
  uuid
  propsys
  dwmapi
  comctl32
  advapi32
  powrprof
  winhttp
)

if(MSVC)
  target_link_options(LeanCast PRIVATE "/MANIFEST:EMBED" "/MANIFESTINPUT:${LEANCAST_MANIFEST}")
endif()

add_executable(LeanCastCoreTests
  native/tests/core_tests.cpp
)

add_library(LeanCastTestExtension SHARED
  native/tests/test_extension.cpp
)

add_library(LeanCastBadApiExtension SHARED
  native/tests/test_extension.cpp
)

add_executable(LeanCastPluginHostTests
  native/tests/plugin_host_tests.cpp
)

target_compile_features(LeanCastCoreTests PRIVATE cxx_std_23)
target_include_directories(LeanCastCoreTests PRIVATE native/src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(LeanCastCoreTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(LeanCastCoreTests PRIVATE LeanCastSQLite advapi32)

target_compile_features(LeanCastPluginHost PRIVATE cxx_std_23)
target_compile_features(LeanCastTestExtension PRIVATE cxx_std_23)
target_compile_features(LeanCastBadApiExtension PRIVATE cxx_std_23)
target_compile_features(LeanCastPluginHostTests PRIVATE cxx_std_23)

target_include_directories(LeanCastTestExtension PRIVATE native/src)
target_include_directories(LeanCastBadApiExtension PRIVATE native/src)
target_include_directories(LeanCastPluginHostTests PRIVATE native/src)

target_compile_definitions(LeanCastBadApiExtension PRIVATE LEANCAST_TEST_BAD_API)
target_compile_definitions(LeanCastPluginHostTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

enable_testing()
add_test(NAME LeanCastCoreTests COMMAND LeanCastCoreTests)
add_test(NAME LeanCastPluginHostTests
  COMMAND LeanCastPluginHostTests
    $<TARGET_FILE:LeanCastPluginHost>
    $<TARGET_FILE:LeanCastTestExtension>
    $<TARGET_FILE:LeanCastBadApiExtension>
)

install(TARGETS LeanCast LeanCastPluginHost RUNTIME DESTINATION bin)

set(CPACK_PACKAGE_NAME "LeanCast")
set(CPACK_PACKAGE_VENDOR "Leon")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME "LeanCast-${PROJECT_VERSION}-win64")
set(CPACK_GENERATOR "ZIP;NSIS")
set(CPACK_NSIS_DISPLAY_NAME "LeanCast")
set(CPACK_NSIS_PACKAGE_NAME "LeanCast")
set(CPACK_PACKAGE_EXECUTABLES "LeanCast" "LeanCast")
set(CPACK_NSIS_MUI_ICON "${LEANCAST_ICON}")
set(CPACK_NSIS_MUI_UNIICON "${LEANCAST_ICON}")
set(CPACK_NSIS_MUI_FINISHPAGE_RUN "LeanCast.exe")
set(CPACK_NSIS_EXTRA_DEFINES "!define MUI_FINISHPAGE_RUN_PARAMETERS \\\"--show\\\"")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
include(CPack)
