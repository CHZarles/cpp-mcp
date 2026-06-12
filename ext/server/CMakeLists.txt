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
set(MCP_EXT_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../common)
include_directories(${MCP_EXT_INCLUDE_DIR})

# Add MCP configuration defines
set(MCP_MAX_SESSIONS 10 CACHE STRING "Maximum concurrent MCP sessions (0 = unlimited)")
set(MCP_SESSION_TIMEOUT 30 CACHE STRING "Inactive session timeout in seconds (0 = disabled)")
option(MCP_BUILD_SYNOLOGY_EXAMPLE "Build Synology NAS MCP example plugin" OFF)
option(MCP_BUILD_CALCULATOR_PLUGIN "Build the calculator example plugin" ON)
add_compile_definitions(
    MCP_MAX_SESSIONS=${MCP_MAX_SESSIONS}
    MCP_SESSION_TIMEOUT=${MCP_SESSION_TIMEOUT}
)

# Main server executable
add_executable(mcp-ext-server
    src/main.cpp
    src/plugin_loader.cpp
    src/plugin_registry.cpp
)

target_link_libraries(mcp-ext-server PRIVATE
    mcp
    nlohmann_json::nlohmann_json
    dl  # For dynamic loading
)

# Default example plugins - smallest built-in tool implementations.
set(EXT_PLUGIN_TARGETS "")
if(MCP_BUILD_CALCULATOR_PLUGIN)
    add_library(calculator SHARED plugins/default/calculator.cpp)
    target_include_directories(calculator PRIVATE ${MCP_EXT_INCLUDE_DIR})
    target_link_libraries(calculator PRIVATE nlohmann_json::nlohmann_json)
    list(APPEND EXT_PLUGIN_TARGETS calculator)
endif()

if(MCP_BUILD_SYNOLOGY_EXAMPLE)
    # Synology plugin family: C++ adapter plus Python backend in plugins/synology.
    find_package(Threads REQUIRED)
    add_library(synology_tools SHARED plugins/synology/synology_tools.cpp)
    target_include_directories(synology_tools PRIVATE
        ${MCP_EXT_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../common  # For httplib.h
    )
    target_link_libraries(synology_tools PRIVATE
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    list(APPEND EXT_PLUGIN_TARGETS synology_tools)
endif()

set_target_properties(${EXT_PLUGIN_TARGETS} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
)

# Install targets
install(TARGETS mcp-ext-server ${EXT_PLUGIN_TARGETS}
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib/plugins
)
