cmake_minimum_required(VERSION 3.10)
project(StaticHttpsServer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find the caduvelox library
find_library(CADUVELOX_LIB caduvelox HINTS ${CMAKE_SOURCE_DIR}/../../build)
find_library(URING_LIB uring REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(static_https_server static_https_server.cpp)

target_include_directories(static_https_server PRIVATE
    ${CMAKE_SOURCE_DIR}/../../include
    ${CMAKE_SOURCE_DIR}/../../external/lock-free-memory-pool/src
)

target_link_libraries(static_https_server
    ${CADUVELOX_LIB}
    ${URING_LIB}
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
)

# Generate self-signed certificates if they don't exist
set(CERT_FILE "${CMAKE_CURRENT_BINARY_DIR}/test_cert.pem")
set(KEY_FILE "${CMAKE_CURRENT_BINARY_DIR}/test_key.pem")

add_custom_command(
    OUTPUT ${CERT_FILE} ${KEY_FILE}
    COMMAND ${CMAKE_COMMAND} -E echo "Generating self-signed certificate..."
    COMMAND openssl req -x509 -newkey rsa:2048 -nodes
        -keyout ${KEY_FILE}
        -out ${CERT_FILE}
        -days 365
        -subj "/C=US/ST=State/L=City/O=Organization/CN=localhost"
    COMMENT "Creating self-signed test certificates for HTTPS server"
)

add_custom_target(generate_certs ALL DEPENDS ${CERT_FILE} ${KEY_FILE})
add_dependencies(static_https_server generate_certs)
