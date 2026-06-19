cmake_minimum_required(VERSION 3.22)

if(NOT WIN32)
  message(FATAL_ERROR "LeanCast Native is a Windows-only Win32 application.")
endif()

project(LeanCastNative VERSION 0.2.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(LEANCAST_ICON "${CMAKE_CURRENT_SOURCE_DIR}/build/icon.ico")
set(LEANCAST_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/native/app.manifest")
configure_file(native/LeanCast.rc.in native/LeanCast.rc @ONLY)

add_executable(LeanCast WIN32
  native/src/main.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/native/LeanCast.rc
)

target_compile_definitions(LeanCast PRIVATE
  UNICODE
  _UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
)

target_include_directories(LeanCast PRIVATE native/src)

target_link_libraries(LeanCast PRIVATE
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
)

if(MSVC)
  target_link_options(LeanCast PRIVATE "/MANIFEST:EMBED" "/MANIFESTINPUT:${LEANCAST_MANIFEST}")
endif()

add_executable(LeanCastCoreTests
  native/tests/core_tests.cpp
)

target_compile_features(LeanCastCoreTests PRIVATE cxx_std_23)
target_include_directories(LeanCastCoreTests PRIVATE native/src)

enable_testing()
add_test(NAME LeanCastCoreTests COMMAND LeanCastCoreTests)

install(TARGETS LeanCast RUNTIME DESTINATION .)

set(CPACK_PACKAGE_NAME "LeanCast")
set(CPACK_PACKAGE_VENDOR "Kurt")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_GENERATOR "ZIP;NSIS")
set(CPACK_NSIS_DISPLAY_NAME "LeanCast")
set(CPACK_NSIS_PACKAGE_NAME "LeanCast")
set(CPACK_PACKAGE_EXECUTABLES "LeanCast" "LeanCast")
include(CPack)
