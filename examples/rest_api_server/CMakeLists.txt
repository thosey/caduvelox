cmake_minimum_required(VERSION 3.10)
project(RestApiServer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find the caduvelox library
find_library(CADUVELOX_LIB caduvelox HINTS ${CMAKE_SOURCE_DIR}/../../build)
find_library(URING_LIB uring REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(rest_api_server rest_api_server.cpp)

target_include_directories(rest_api_server PRIVATE
    ${CMAKE_SOURCE_DIR}/../../include
    ${CMAKE_SOURCE_DIR}/../../external/lock-free-memory-pool/src
)

target_link_libraries(rest_api_server
    ${CADUVELOX_LIB}
    ${URING_LIB}
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
)
