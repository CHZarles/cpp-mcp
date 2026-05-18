cmake_minimum_required(VERSION 3.14)
project(mcp-ext-server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find nlohmann_json
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.2
    )
    FetchContent_MakeAvailable(json)
endif()

# Include cpp-mcp headers
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../common)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/plugins)

# Add MCP configuration defines
set(MCP_MAX_SESSIONS 10 CACHE STRING "Maximum concurrent MCP sessions (0 = unlimited)")
set(MCP_SESSION_TIMEOUT 30 CACHE STRING "Inactive session timeout in seconds (0 = disabled)")
add_compile_definitions(
    MCP_MAX_SESSIONS=${MCP_MAX_SESSIONS}
    MCP_SESSION_TIMEOUT=${MCP_SESSION_TIMEOUT}
)

# Main server executable
add_executable(mcp-ext-server
    src/main.cpp
    src/plugin_loader.cpp
)

target_link_libraries(mcp-ext-server PRIVATE
    mcp
    nlohmann_json::nlohmann_json
    dl  # For dynamic loading
)

# Example plugins (legacy, using old API - kept for reference)
add_library(calculator SHARED plugins/calculator.cpp)
target_include_directories(calculator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/plugins)
target_link_libraries(calculator PRIVATE nlohmann_json::nlohmann_json)

add_library(weather SHARED plugins/weather.cpp)
target_include_directories(weather PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/plugins)
target_link_libraries(weather PRIVATE nlohmann_json::nlohmann_json)

# WSL Tool Plugins (one .so = one tool)
add_library(wsl_create_directory SHARED plugins/wsl_create_directory.cpp)
target_include_directories(wsl_create_directory PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/plugins)
target_link_libraries(wsl_create_directory PRIVATE nlohmann_json::nlohmann_json)

add_library(wsl_list_distros SHARED plugins/wsl_list_distros.cpp)
target_include_directories(wsl_list_distros PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/plugins)
target_link_libraries(wsl_list_distros PRIVATE nlohmann_json::nlohmann_json)

# Install plugins to build/plugins directory
set_target_properties(calculator weather wsl_create_directory wsl_list_distros PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
)

# Install targets
install(TARGETS mcp-ext-server calculator weather wsl_create_directory wsl_list_distros
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib/plugins
)