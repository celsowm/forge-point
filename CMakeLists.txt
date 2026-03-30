cmake_minimum_required(VERSION 3.21)
project(ForgePoint VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DEPS_DIR "${CMAKE_SOURCE_DIR}/build/_deps")

if(EXISTS "${DEPS_DIR}/ftxui-src/CMakeLists.txt" AND EXISTS "${DEPS_DIR}/json-src/CMakeLists.txt")
  message(STATUS "Using cached dependencies from ${DEPS_DIR}")
  set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory("${DEPS_DIR}/ftxui-src" "${DEPS_DIR}/ftxui-build" EXCLUDE_FROM_ALL)
  set(JSON_Install OFF CACHE BOOL "" FORCE)
  add_subdirectory("${DEPS_DIR}/json-src" "${DEPS_DIR}/json-build" EXCLUDE_FROM_ALL)
else()
  message(STATUS "Fetching dependencies (first build)...")
  include(FetchContent)
  FetchContent_Declare(
    ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG v6.1.9
  )
  FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
  )
  FetchContent_Declare(
    ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG v6.1.9
  )
  FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
  )
  FetchContent_MakeAvailable(ftxui json)
endif()

add_executable(forge_point
  # Entry point
  ${CMAKE_SOURCE_DIR}/src/main.cpp

  # Core layer
  ${CMAKE_SOURCE_DIR}/src/core/string_utils.cpp
  ${CMAKE_SOURCE_DIR}/src/core/platform_utils.cpp
  ${CMAKE_SOURCE_DIR}/src/core/http_client.cpp
  ${CMAKE_SOURCE_DIR}/src/core/process_supervisor.cpp
  ${CMAKE_SOURCE_DIR}/src/core/gpu_detector.cpp
  ${CMAKE_SOURCE_DIR}/src/core/cpu_detector.cpp
  ${CMAKE_SOURCE_DIR}/src/core/llama_server_manager.cpp
  ${CMAKE_SOURCE_DIR}/src/core/llama_downloader.cpp

  # Data layer
  ${CMAKE_SOURCE_DIR}/src/data/hf_client.cpp
  ${CMAKE_SOURCE_DIR}/src/data/hf_cache_scanner.cpp

  # App layer
  ${CMAKE_SOURCE_DIR}/src/app/app.cpp
  ${CMAKE_SOURCE_DIR}/src/app/logger.cpp
  ${CMAKE_SOURCE_DIR}/src/app/rotating_file_logger.cpp
  ${CMAKE_SOURCE_DIR}/src/app/transfer_tracker.cpp
  ${CMAKE_SOURCE_DIR}/src/app/command_registry.cpp
  ${CMAKE_SOURCE_DIR}/src/app/command_handler.cpp
  ${CMAKE_SOURCE_DIR}/src/app/confirmation_guard.cpp
  ${CMAKE_SOURCE_DIR}/src/app/debug_overlay.cpp

  # UI layer
  ${CMAKE_SOURCE_DIR}/src/ui/welcome_screen.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/home_menu.cpp
)

target_include_directories(forge_point PRIVATE ${CMAKE_SOURCE_DIR}/src)

set_target_properties(forge_point PROPERTIES
  OUTPUT_NAME "forge-point"
)

target_link_libraries(forge_point PRIVATE
  ftxui::component
  ftxui::dom
  ftxui::screen
  nlohmann_json::nlohmann_json
)



if(WIN32)
  target_compile_definitions(forge_point PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(forge_point PRIVATE winhttp dxgi)
  # Vulkan for fallback GPU detection
  find_package(Vulkan QUIET)
  if(Vulkan_FOUND)
    target_link_libraries(forge_point PRIVATE Vulkan::Vulkan)
    target_compile_definitions(forge_point PRIVATE HAS_VULKAN)
  endif()
elseif(APPLE)
  target_compile_definitions(forge_point PRIVATE NOMINMAX)
  target_link_libraries(forge_point PRIVATE "-framework Metal" "-framework QuartzCore")
  # Vulkan via MoltenVK on macOS
  find_package(Vulkan QUIET)
  if(Vulkan_FOUND)
    target_link_libraries(forge_point PRIVATE Vulkan::Vulkan)
    target_compile_definitions(forge_point PRIVATE HAS_VULKAN)
  endif()
else()
  # Linux
  target_compile_definitions(forge_point PRIVATE NOMINMAX)
  target_link_libraries(forge_point PRIVATE pthread)
  find_package(Vulkan QUIET)
  if(Vulkan_FOUND)
    target_link_libraries(forge_point PRIVATE Vulkan::Vulkan)
    target_compile_definitions(forge_point PRIVATE HAS_VULKAN)
  endif()
endif()

# ─── Tests ─────────────────────────────────────────────────────────────────────
enable_testing()

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# Test executable
add_executable(forge_point_tests
  # App source files (for linking)
  ${CMAKE_SOURCE_DIR}/src/app/logger.cpp
  ${CMAKE_SOURCE_DIR}/src/app/rotating_file_logger.cpp
  ${CMAKE_SOURCE_DIR}/src/app/transfer_tracker.cpp
  ${CMAKE_SOURCE_DIR}/src/app/command_registry.cpp
  ${CMAKE_SOURCE_DIR}/src/app/command_handler.cpp
  ${CMAKE_SOURCE_DIR}/src/app/confirmation_guard.cpp
  ${CMAKE_SOURCE_DIR}/src/core/string_utils.cpp
  
  # Unit tests
  tests/unit/command_handler_test.cpp
  tests/unit/confirmation_guard_test.cpp
  tests/unit/transfer_tracker_test.cpp
  tests/unit/command_registry_test.cpp
  
  # Component tests
  tests/component/menu_navigation_test.cpp
)

target_link_libraries(forge_point_tests PRIVATE
  GTest::gtest_main
  GTest::gmock_main
  ftxui::component
  ftxui::dom
  ftxui::screen
  nlohmann_json::nlohmann_json
  winhttp
)

target_include_directories(forge_point_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(WIN32)
  target_compile_definitions(forge_point_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

gtest_discover_tests(forge_point_tests)
