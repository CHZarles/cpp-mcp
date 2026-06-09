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
option(MCP_BUILD_SYNOLOGY_EXAMPLE "Build Synology NAS MCP example plugin" OFF)
add_compile_definitions(
    MCP_MAX_SESSIONS=${MCP_MAX_SESSIONS}
    MCP_SESSION_TIMEOUT=${MCP_SESSION_TIMEOUT}
)

# Main server executable
add_executable(mcp-ext-server
    src/main.cpp
    src/plugin_loader.cpp
    src/plugin_registry.cpp
    src/wsl_resources.cpp
)

target_link_libraries(mcp-ext-server PRIVATE
    mcp
    nlohmann_json::nlohmann_json
    dl  # For dynamic loading
)

# Example plugin
add_library(calculator SHARED plugins/calculator.cpp)
target_include_directories(calculator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/plugins)
target_link_libraries(calculator PRIVATE nlohmann_json::nlohmann_json)

set(EXT_PLUGIN_TARGETS calculator)

if(MCP_BUILD_SYNOLOGY_EXAMPLE)
    # Synology API backend adapter plugin used by examples/synology-nas.
    find_package(Threads REQUIRED)
    add_library(synology_tools SHARED plugins/synology_tools.cpp)
    target_include_directories(synology_tools PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/plugins
        ${CMAKE_CURRENT_SOURCE_DIR}/../../common  # For httplib.h
    )
    target_link_libraries(synology_tools PRIVATE
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    list(APPEND EXT_PLUGIN_TARGETS synology_tools)
endif()

# WSL Tools - combined plugin (one .so with multiple tools)
set(WSL_TOOLS_SOURCES
    plugins/wsl_tools/wsl_tools.cpp
    plugins/wsl_tools/wsl_create_directory.cpp
    plugins/wsl_tools/wsl_list_distros.cpp
    plugins/wsl_tools/wsl_scan_files.cpp
    plugins/wsl_tools/wsl_scan_read.cpp
    plugins/wsl_tools/wsl_recommend_cleanup.cpp
    plugins/wsl_tools/wsl_safe_delete.cpp
)
add_library(wsl_tools SHARED ${WSL_TOOLS_SOURCES})
target_include_directories(wsl_tools PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/plugins
    ${CMAKE_CURRENT_SOURCE_DIR}/plugins/wsl_tools  # For relative includes like ../plugins/tool_api.h
)
target_link_libraries(wsl_tools PRIVATE nlohmann_json::nlohmann_json)

# Install plugins to build/plugins directory
list(APPEND EXT_PLUGIN_TARGETS wsl_tools)
set_target_properties(${EXT_PLUGIN_TARGETS} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
)

# Install targets
install(TARGETS mcp-ext-server ${EXT_PLUGIN_TARGETS}
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib/plugins
)
