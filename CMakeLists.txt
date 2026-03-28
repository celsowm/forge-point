cmake_minimum_required(VERSION 3.21)
project(ForgePoint VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

FetchContent_MakeAvailable(ftxui json)
find_package(CURL REQUIRED)

add_executable(forge_point
  src/main.cpp
)

set_target_properties(forge_point PROPERTIES
  OUTPUT_NAME "forge-point"
)

target_link_libraries(forge_point PRIVATE
  ftxui::component
  ftxui::dom
  ftxui::screen
  nlohmann_json::nlohmann_json
  CURL::libcurl
)

if(WIN32)
  target_compile_definitions(forge_point PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
