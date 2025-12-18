cmake_minimum_required(VERSION 3.10)
project(Caduvelox)

# Enable compile_commands.json for clangd and LSPs
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Option to enable sanitizers (ASAN/UBSAN) for local/CI debugging
option(SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(SANITIZERS)
    message(STATUS "SANITIZERS enabled: adding ASAN/UBSAN flags")
    # Add sanitizer flags for C and C++ and to the linker
    set(SANITIZER_FLAGS "-fsanitize=address,undefined -fno-omit-frame-pointer -g")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${SANITIZER_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SANITIZER_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZER_FLAGS}")
endif()

# Find liburing
find_library(URING_LIB uring REQUIRED)

# Find system OpenSSL (requires kTLS support)
find_package(OpenSSL REQUIRED)

# Verify OpenSSL has kTLS support
message(STATUS "Found OpenSSL ${OPENSSL_VERSION}")
message(STATUS "OpenSSL include dir: ${OPENSSL_INCLUDE_DIR}")
message(STATUS "OpenSSL libraries: ${OPENSSL_LIBRARIES}")

add_library(caduvelox
    # Core infrastructure
    src/Server.cpp
    src/ServiceRing.cpp
    src/logging/Logger.cpp
    src/logging/AsyncLogger.cpp
    src/logging/ConsoleLogger.cpp
    src/logging/FileLogger.cpp
    # Buffer management for zero-copy operations
    src/ring_buffer/BufferRingCoordinator.cpp
    # Job-based architecture (low-level io_uring jobs)
    src/jobs/WriteJob.cpp
    src/jobs/AcceptJob.cpp
    src/jobs/SpliceFileJob.cpp
    src/jobs/KTLSJob.cpp
    src/jobs/KTLSContextHelper.cpp
    # HTTP layer (server, routing, file serving)
    src/http/HttpParser.cpp
    src/http/HttpRouter.cpp
    src/http/HttpServer.cpp             # Multi-ring coordinator (public API)
    src/http/SingleRingHttpServer.cpp   # Internal single-ring implementation
    src/http/HttpConnectionRecvHandler.cpp
    src/http/HTTPFileJob.cpp
    src/threading/SimpleWorkerPool.cpp
)

target_include_directories(caduvelox PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(caduvelox 
    ${URING_LIB} 
    OpenSSL::SSL 
    OpenSSL::Crypto
)

# Examples are now in individual folders with their own CMakeLists.txt
# Optionally add them as subdirectories (excluded from default build)
option(BUILD_EXAMPLES "Build example applications" OFF)
if(BUILD_EXAMPLES)
    add_subdirectory(examples/static_https_server EXCLUDE_FROM_ALL)
    add_subdirectory(examples/rest_api_server EXCLUDE_FROM_ALL)
endif()

# Add tests
enable_testing()
add_subdirectory(tests)
