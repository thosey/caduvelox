# Generate self-signed certificate and key for TLS tests if not present
set(TEST_CERT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/test_cert.pem)
set(TEST_KEY_SRC ${CMAKE_CURRENT_SOURCE_DIR}/test_key.pem)
set(TEST_CERT_BUILD ${CMAKE_CURRENT_BINARY_DIR}/test_cert.pem)
set(TEST_KEY_BUILD ${CMAKE_CURRENT_BINARY_DIR}/test_key.pem)

add_custom_command(
    OUTPUT ${TEST_CERT_SRC} ${TEST_KEY_SRC}
    COMMAND openssl req -x509 -newkey rsa:2048 -keyout ${TEST_KEY_SRC} -out ${TEST_CERT_SRC} -days 365 -nodes -subj "/CN=localhost"
    COMMENT "Generating self-signed TLS certificate and key for tests..."
    VERBATIM
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

# Copy certificates to build directory so tests can find them
add_custom_command(
    OUTPUT ${TEST_CERT_BUILD} ${TEST_KEY_BUILD}
    COMMAND ${CMAKE_COMMAND} -E copy ${TEST_CERT_SRC} ${TEST_CERT_BUILD}
    COMMAND ${CMAKE_COMMAND} -E copy ${TEST_KEY_SRC} ${TEST_KEY_BUILD}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/tests
    DEPENDS ${TEST_CERT_SRC} ${TEST_KEY_SRC}
    COMMENT "Copying TLS certificates to build directory..."
)

# Job Architecture Tests - Temporarily disabled during refactoring
# add_executable(job_architecture_test
#     job_architecture_test.cpp
#     ${TEST_CERT_BUILD}
#     ${TEST_KEY_BUILD}
# )

# target_link_libraries(job_architecture_test
#     caduvelox
#     ${URING_LIB}
#     OpenSSL::SSL
#     OpenSSL::Crypto
#     GTest::gtest
#     GTest::gtest_main
#     pthread
# )

# Simple Job Server Tests - Temporarily disabled during refactoring
# add_executable(simple_job_test
#     simple_job_test.cpp
#     ${TEST_CERT_BUILD}
#     ${TEST_KEY_BUILD}
# )

# target_link_libraries(simple_job_test
#     caduvelox
#     ${URING_LIB}
#     OpenSSL::SSL
#     OpenSSL::Crypto
#     GTest::gtest
#     GTest::gtest_main
#     pthread
# )

add_custom_target(generate_test_cert_key ALL DEPENDS ${TEST_CERT_BUILD} ${TEST_KEY_BUILD})

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED liburing)

# Find Google Test
find_package(GTest REQUIRED)
message(STATUS "Found GTest: ${GTEST_INCLUDE_DIRS}")

# Include directories
include_directories(../src)
include_directories(${GTEST_INCLUDE_DIRS})
include_directories(${CMAKE_SOURCE_DIR}/include)


# Create a single test executable for all tests in this folder
set(TEST_SOURCES
    test_main.cpp
    # Utility/Data Structure Tests (architecture-agnostic)
    util/ring_buffer/vyukov_ring_buffer_test.cpp
    util/ring_buffer/notifying_ring_buffer_test.cpp
    # Router Unit Tests (architecture-agnostic)
    http_router_unit_test.cpp
    # Logger Tests (file logging and rotation)
    logger_test.cpp
    # Job-Based Architecture Tests (primary architecture)
    job_http_server_test.cpp
    job_http_pipelining_test.cpp
    job_ktls_http_pipelining_test.cpp
    job_ktls_http_send_file_test.cpp
    # Static HTTPS Server Tests
    static_https_server_test.cpp
    # Security Regression Tests (issues #1-#10 from audit)
    security_regression_test.cpp
)
add_executable(caduvelox_tests ${TEST_SOURCES})
add_dependencies(caduvelox_tests generate_test_cert_key)

# Create test executable for HTTP server
#add_executable(http_server_tests
#    http_server_test.cpp
#)



# Link libraries
target_link_libraries(caduvelox_tests 
    caduvelox
    GTest::GTest
    GTest::Main
    ${LIBURING_LIBRARIES}
    pthread
)

#target_link_libraries(http_server_tests 
#    ${GTEST_LIBRARIES}
#    ${LIBURING_LIBRARIES}
#    pthread
#)

# Add compiler flags
target_compile_options(caduvelox_tests PRIVATE ${LIBURING_CFLAGS})
#target_compile_options(http_server_tests PRIVATE ${LIBURING_CFLAGS})

# Enable testing
enable_testing()

# Add the main test executable
add_test(NAME CaduveloxTests COMMAND caduvelox_tests)

# Add individual test suites for better granularity
add_test(NAME RingBufferTests COMMAND caduvelox_tests --gtest_filter="*RingBufferTest.*")
add_test(NAME HttpServerTests COMMAND caduvelox_tests --gtest_filter="HttpServerTest.*")
add_test(NAME HttpParserTests COMMAND caduvelox_tests --gtest_filter="HttpParser*Test.*")
add_test(NAME HttpRouterTests COMMAND caduvelox_tests --gtest_filter="HttpRouterUnitTest.*")
add_test(NAME WriteMappingTests COMMAND caduvelox_tests --gtest_filter="WriteMappingRegression.*")
add_test(NAME HttpPipeliningTests COMMAND caduvelox_tests --gtest_filter="HttpPipeliningTest.*")
add_test(NAME JobHttpServerTests COMMAND caduvelox_tests --gtest_filter="JobHttpServerTest.*")

# Job-based architecture tests
add_test(NAME JobHttpPipeliningTests COMMAND caduvelox_tests --gtest_filter="JobHttpPipeliningTest.*")
add_test(NAME JobKTLSPipeliningTests COMMAND caduvelox_tests --gtest_filter="JobKTLSPipeliningTest.*")
add_test(NAME JobKTLSSendFileTests COMMAND caduvelox_tests --gtest_filter="JobKTLSSendFileTest.*")
add_test(NAME StaticHttpsServerTests COMMAND caduvelox_tests --gtest_filter="StaticHttpsServerTest.*")
add_test(NAME SecurityRegressionTests COMMAND caduvelox_tests --gtest_filter="SecurityRegressionTest.*")

# Add KTLS tests separately (these might fail in CI)
add_test(NAME KTLSTests COMMAND caduvelox_tests "--gtest_filter=KTLS*")
set_tests_properties(KTLSTests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
