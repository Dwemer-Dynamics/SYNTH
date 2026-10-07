cmake_minimum_required(VERSION 3.25)

project(SYNTH VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/DependencyPins.cmake")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SYNTH_BUILD_CORE_TESTS "Build engine-free native tests" ON)
option(SYNTH_BUILD_VR "Build the independent SYNTHVR plugin lane" OFF)
option(SYNTH_FETCH_VR_DEPENDENCIES "Fetch the pinned CommonLibF4VR source" OFF)
set(SYNTH_COMMONLIBF4VR_SOURCE_DIR "" CACHE PATH "Existing pinned CommonLibF4VR checkout")

add_library(synth_core INTERFACE)
target_include_directories(synth_core INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(synth_core INTERFACE cxx_std_23)

if(SYNTH_BUILD_CORE_TESTS)
  set(_synth_test_suites
    native
    transport
    json
    fake-server
    config
    diagnostics
    tasks
    lifecycle
    context
    targeting
    actions
    input
    audio
    presentation
    media-fetch
    adapters)
  set(_synth_test_sources
    "tests/native/native_tests.cpp"
    "tests/transport/transport_tests.cpp"
    "tests/json/json_tests.cpp"
    "tests/fake_server/fake_server_tests.cpp"
    "tests/config/config_tests.cpp"
    "tests/diagnostics/diagnostics_tests.cpp"
    "tests/tasks/task_lanes_tests.cpp"
    "tests/lifecycle/lifecycle_tests.cpp"
    "tests/context/context_tests.cpp"
    "tests/targeting/targeting_tests.cpp"
    "tests/actions/actions_tests.cpp"
    "tests/input/input_tests.cpp"
    "tests/audio/audio_tests.cpp"
    "tests/presentation/presentation_tests.cpp"
    "tests/media_fetch/media_fetch_tests.cpp"
    "tests/adapters/adapters_tests.cpp")

  set(_synth_protocol_native_source "${CMAKE_CURRENT_SOURCE_DIR}/tests/protocol_native/protocol_native_tests.cpp")
  if(EXISTS "${_synth_protocol_native_source}")
    list(APPEND _synth_test_suites protocol-native)
    list(APPEND _synth_test_sources "${_synth_protocol_native_source}")
  endif()

  find_package(Threads REQUIRED)
  list(LENGTH _synth_test_suites _synth_test_suite_count)
  math(EXPR _synth_test_suite_last "${_synth_test_suite_count} - 1")
  foreach(_synth_test_index RANGE ${_synth_test_suite_last})
    list(GET _synth_test_suites ${_synth_test_index} _synth_test_suite)
    list(GET _synth_test_sources ${_synth_test_index} _synth_test_source)
    set(_synth_test_target "synth-${_synth_test_suite}-tests")

    add_executable("${_synth_test_target}" "${_synth_test_source}")
    target_link_libraries("${_synth_test_target}" PRIVATE synth_core)
    if(_synth_test_suite STREQUAL "native" OR
       _synth_test_suite STREQUAL "tasks" OR
       _synth_test_suite STREQUAL "lifecycle" OR
       _synth_test_suite STREQUAL "adapters")
      target_link_libraries("${_synth_test_target}" PRIVATE Threads::Threads)
    endif()
    if(MSVC)
      target_compile_options("${_synth_test_target}" PRIVATE /W4 /permissive- /EHsc)
    else()
      target_compile_options("${_synth_test_target}" PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(_synth_test_suite STREQUAL "protocol-native")
      add_test(NAME "${_synth_test_target}" COMMAND "${_synth_test_target}" "${CMAKE_CURRENT_SOURCE_DIR}")
    else()
      add_test(NAME "${_synth_test_target}" COMMAND "${_synth_test_target}")
    endif()
    set_tests_properties("${_synth_test_target}" PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  endforeach()
endif()

if(SYNTH_BUILD_VR)
  if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "SYNTHVR.dll requires a Windows x64 toolchain")
  endif()

  set(_synth_vr_entry "${CMAKE_CURRENT_SOURCE_DIR}/src/vr/plugin_entry.cpp")
  if(NOT EXISTS "${_synth_vr_entry}")
    message(FATAL_ERROR
      "SYNTHVR lane requested, but src/vr/plugin_entry.cpp does not exist. "
      "No DLL will be fabricated; implement the VR entry point first.")
  endif()

  if(NOT CMAKE_TOOLCHAIN_FILE)
    message(FATAL_ERROR
      "SYNTHVR requires vcpkg integration. Configure with "
      "-DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake "
      "-DVCPKG_TARGET_TRIPLET=x64-windows-static-md and install "
      "rsm-mmio:x64-windows-static-md and spdlog:x64-windows-static-md.")
  endif()

  find_package(mmio CONFIG QUIET)
  find_package(spdlog CONFIG QUIET)
  if(NOT TARGET mmio::mmio OR NOT TARGET spdlog::spdlog)
    message(FATAL_ERROR
      "SYNTHVR requires the vcpkg packages rsm-mmio:x64-windows-static-md and "
      "spdlog:x64-windows-static-md so the shipped DLL does not import "
      "spdlog.dll or fmt.dll. Install them with "
      "vcpkg install rsm-mmio:x64-windows-static-md spdlog:x64-windows-static-md, "
      "then configure with the vcpkg CMAKE_TOOLCHAIN_FILE.")
  endif()

  # These options must be set before CommonLibF4VR is fetched or added so its
  # configure step cannot retain the flat/NG defaults or enable dependency tests.
  set(ENABLE_FALLOUT_F4 OFF CACHE BOOL "Build flat Fallout 4 support" FORCE)
  set(ENABLE_FALLOUT_NG OFF CACHE BOOL "Build next-gen Fallout 4 support" FORCE)
  set(ENABLE_FALLOUT_VR ON CACHE BOOL "Build Fallout 4 VR support" FORCE)
  set(BUILD_TESTS OFF CACHE BOOL "Build dependency tests" FORCE)

  if(SYNTH_COMMONLIBF4VR_SOURCE_DIR)
    get_filename_component(_synth_commonlibf4vr "${SYNTH_COMMONLIBF4VR_SOURCE_DIR}" ABSOLUTE)
  elseif(SYNTH_FETCH_VR_DEPENDENCIES)
    include(FetchContent)
    FetchContent_Declare(commonlibf4vr
      GIT_REPOSITORY "${SYNTH_COMMONLIBF4VR_REPOSITORY}"
      GIT_TAG "${SYNTH_COMMONLIBF4VR_REVISION}"
      GIT_SHALLOW FALSE
      GIT_SUBMODULES_RECURSE TRUE)
    FetchContent_MakeAvailable(commonlibf4vr)
    set(_synth_commonlibf4vr "${commonlibf4vr_SOURCE_DIR}")
  else()
    message(FATAL_ERROR
      "SYNTHVR requires the pinned CommonLibF4VR source. Set "
      "SYNTH_COMMONLIBF4VR_SOURCE_DIR or enable SYNTH_FETCH_VR_DEPENDENCIES. "
      "F4SEVR 0.6.21 and VR Address Library v1.13.1 are required external runtime installs, not vendored inputs.")
  endif()

  if(NOT TARGET CommonLibF4)
    if(NOT EXISTS "${_synth_commonlibf4vr}/CommonLibF4/CMakeLists.txt")
      message(FATAL_ERROR
        "SYNTH_COMMONLIBF4VR_SOURCE_DIR must name the pinned CommonLibF4VR repository root; "
        "CommonLibF4/CMakeLists.txt was not found under ${_synth_commonlibf4vr}.")
    endif()
    add_subdirectory("${_synth_commonlibf4vr}/CommonLibF4" "_deps/commonlibf4vr-build" EXCLUDE_FROM_ALL)
  endif()

  add_library(SYNTHVR SHARED
    "${_synth_vr_entry}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/adapter_descriptor.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/adapter_lifecycle.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/commonlib_actions.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/commonlib_capture.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/client/plugin_session.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/detail/runtime_base.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/vr_fallout_runtime.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/settings_catalog.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/settings_store.hpp")
  target_link_libraries(SYNTHVR PRIVATE synth_core CommonLibF4::CommonLibF4 winhttp xaudio2 winmm
    shell32 windowscodecs ole32 oleaut32 gdi32)
  target_compile_definitions(SYNTHVR PRIVATE SYNTH_WITH_F4SEVR=1 WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00 WINVER=0x0A00)
  set_target_properties(SYNTHVR PROPERTIES PREFIX "" OUTPUT_NAME "SYNTHVR")
endif()
