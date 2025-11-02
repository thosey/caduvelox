# REST API Server Example
# 
# This CMakeLists.txt supports two build modes:
# 1. As a subdirectory of the main project (BUILD_EXAMPLES=ON)
# 2. As a standalone project (users can copy this to their own projects)

cmake_minimum_required(VERSION 3.10)

# Detect if we're being built as part of the main project or standalone
if(NOT TARGET caduvelox)
    # Standalone mode - set up project and find caduvelox library
    project(RestApiServer)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    
    # Find the built caduvelox library
    set(CADUVELOX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Path to caduvelox root directory")
    
    find_library(CADUVELOX_LIB 
        NAMES caduvelox libcaduvelox.a
        HINTS "${CADUVELOX_ROOT}/build"
        REQUIRED
    )
    
    set(CADUVELOX_INCLUDE_DIR "${CADUVELOX_ROOT}/include")
    set(LOCKFREE_INCLUDE_DIR "${CADUVELOX_ROOT}/external/lock-free-memory-pool/src")
    
    message(STATUS "Standalone build mode")
    message(STATUS "Found caduvelox library: ${CADUVELOX_LIB}")
else()
    # Subdirectory mode - use caduvelox target directly
    message(STATUS "Subdirectory build mode")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing)
find_package(OpenSSL REQUIRED)

add_executable(rest_api_server rest_api_server.cpp)

if(NOT TARGET caduvelox)
    # Standalone: use found library and include paths
    target_include_directories(rest_api_server PRIVATE
        ${CADUVELOX_INCLUDE_DIR}
        ${LOCKFREE_INCLUDE_DIR}
    )
    
    target_link_libraries(rest_api_server PRIVATE
        ${CADUVELOX_LIB}
        PkgConfig::LIBURING
        OpenSSL::SSL
        OpenSSL::Crypto
        pthread
    )
else()
    # Subdirectory: use caduvelox target
    target_include_directories(rest_api_server PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/external/lock-free-memory-pool/src
    )
    
    target_link_libraries(rest_api_server PRIVATE
        caduvelox
        PkgConfig::LIBURING
        OpenSSL::SSL
        OpenSSL::Crypto
        pthread
    )
endif()
