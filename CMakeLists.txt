cmake_minimum_required(VERSION 3.24)
project(NVSmooth30 VERSION 0.1.0 LANGUAGES CXX)

option(NVS30_BUILD_TESTS "Build the portable fatbin unit tests" ON)
option(NVS30_BUILD_PROBE "Build the GPU smoke-test probe (Windows only)" ${WIN32})

if(NOT WIN32 OR NOT MSVC)
  if(NOT NVS30_BUILD_TESTS)
    message(FATAL_ERROR "NVSmooth30 currently targets 64-bit Windows with MSVC")
  endif()
  message(STATUS "Non-Windows host: building portable unit-test targets only")
endif()

if(WIN32 AND MSVC)
add_library(NVSmooth30 SHARED
  src/dllmain.cpp
  src/config.cpp
  src/gpu.cpp
  src/log.cpp
  src/pe.cpp
  src/fatbin.cpp
  src/nvpresent.cpp
  src/dxgi_hooks.cpp
  src/d3d11_bridge.cpp
  src/osd.cpp
  src/pacer.cpp
  src/version_proxy.cpp
  src/version_proxy.def
)

target_include_directories(NVSmooth30 PRIVATE include)
target_compile_features(NVSmooth30 PRIVATE cxx_std_20)
target_compile_definitions(NVSmooth30 PRIVATE
  WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
  NVS30_VERSION="${PROJECT_VERSION}"
)
target_compile_options(NVSmooth30 PRIVATE /W4 /permissive- /EHsc /guard:cf-)
target_link_options(NVSmooth30 PRIVATE /DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/version_proxy.def)
target_link_libraries(NVSmooth30 PRIVATE
  d3d11 d3d12 dxgi dxguid user32 gdi32 shlwapi
)
set_target_properties(NVSmooth30 PROPERTIES OUTPUT_NAME version)

install(TARGETS NVSmooth30 RUNTIME DESTINATION . LIBRARY DESTINATION .)

if(NVS30_BUILD_PROBE)
  # RTX 2060 smoke-test harness: a plain D3D11 window app used to validate the
  # injection + Present-hook + fatbin pipeline end to end on real hardware.
  add_executable(nvs30_probe tools/probe/probe.cpp)
  target_compile_features(nvs30_probe PRIVATE cxx_std_20)
  target_link_libraries(nvs30_probe PRIVATE d3d11 dxgi user32 shell32)
  set_target_properties(nvs30_probe PROPERTIES OUTPUT_NAME nvs30_probe)
  install(TARGETS nvs30_probe RUNTIME DESTINATION .)
endif()
endif()

if(NVS30_BUILD_TESTS)
  enable_testing()
  # The fatbin engine is host-portable by design; these tests run the exact
  # same core logic on Linux and Windows CI with real ptxas-generated cubins
  # embedded as fixtures (no NVIDIA hardware required).
  add_executable(fatbin_tests
    tests/fatbin_tests.cpp
    src/fatbin.cpp
  )
  target_include_directories(fatbin_tests PRIVATE include tests)
  target_compile_features(fatbin_tests PRIVATE cxx_std_20)
  if(MSVC)
    target_compile_options(fatbin_tests PRIVATE /W4 /permissive- /EHsc)
  else()
    target_compile_options(fatbin_tests PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME fatbin_tests COMMAND fatbin_tests)
endif()
