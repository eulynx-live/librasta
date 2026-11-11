
include(FindPkgConfig)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(CUnit IMPORTED_TARGET cunit)
endif(PKG_CONFIG_FOUND)

add_executable(rasta_test
    ../examples/common/c/configfile.c
    ../examples/common/c/dictionary.c
    rasta_test/headers/blake2_test.h
    rasta_test/headers/config_test.h
    rasta_test/headers/dictionary_test.h
    rasta_test/headers/fifo_test.h
    rasta_test/headers/rastacrc_test.h
    rasta_test/headers/rastadeferqueue_test.h
    rasta_test/headers/rastafactory_test.h
    rasta_test/headers/rastamd4_test.h
    rasta_test/headers/rastamodule_test.h
    rasta_test/headers/register_tests.h
    rasta_test/headers/siphash24_test.h
    rasta_test/headers/opaque_test.h
    rasta_test/headers/redundancy_channel_test.h
    rasta_test/headers/safety_retransmission_test.h
    rasta_test/c/blake2_test.c
    rasta_test/c/config_test.c
    rasta_test/c/dictionary_test.c
    rasta_test/c/fifo_test.c
    rasta_test/c/rastacrc_test.c
    rasta_test/c/rastadeferqueue_test.c
    rasta_test/c/rastafactory_test.c
    rasta_test/c/rastamd4_test.c
    rasta_test/c/rastamodule_test.c
    rasta_test/c/register_tests.c
    rasta_test/c/siphash24_test.c
    rasta_test/c/opaque_test.c
    rasta_test/c/redundancy_channel_test.c
    rasta_test/c/safety_retransmission_test.c
)
target_include_directories(rasta_test PRIVATE rasta_test/headers ../examples/common/headers)
target_link_libraries(rasta_test rasta_udp PkgConfig::CUnit)
target_compile_definitions(rasta_test PRIVATE WITH_CMAKE)

add_dependencies(rasta_test rasta_udp)

add_test(NAME test_rasta_test
         COMMAND rasta_test)

if(ENABLE_RASTA_TLS)
    set(RASTA_VARIANTS udp tcp dtls tls)
else()
    set(RASTA_VARIANTS udp tcp)
endif(ENABLE_RASTA_TLS)

foreach(RASTA_VARIANT ${RASTA_VARIANTS})
    if(${RASTA_VARIANT} STREQUAL "tls")
        set(TEST_VARIANT tcp)
    elseif(${RASTA_VARIANT} STREQUAL "dtls")
        set(TEST_VARIANT udp)
    else()
        set(TEST_VARIANT ${RASTA_VARIANT})
    endif()
    add_executable(rasta_transport_test_${RASTA_VARIANT}
        rasta_transport_test/headers/register_tests.h
        rasta_transport_test/headers/mock_socket.h
        rasta_transport_test/headers/transport_test.h
        rasta_transport_test/headers/transport_test_${TEST_VARIANT}.h
        rasta_transport_test/c/register_tests.c
        rasta_transport_test/c/mock_socket.c
        rasta_transport_test/c/transport_test.c
        rasta_transport_test/c/transport_test_${TEST_VARIANT}.c
    )

    target_include_directories(rasta_transport_test_${RASTA_VARIANT} PRIVATE rasta_transport_test/headers)
    target_link_libraries(rasta_transport_test_${RASTA_VARIANT} rasta_${RASTA_VARIANT} PkgConfig::CUnit)
    target_compile_definitions(rasta_transport_test_${RASTA_VARIANT} PRIVATE WITH_CMAKE)

    add_dependencies(rasta_transport_test_${RASTA_VARIANT} rasta_${RASTA_VARIANT})

    add_test(NAME test_rasta_transport_test_${RASTA_VARIANT}
            COMMAND rasta_transport_test_${RASTA_VARIANT})
endforeach()

target_compile_definitions(rasta_transport_test_udp PUBLIC TEST_UDP)
target_compile_definitions(rasta_transport_test_tcp PUBLIC TEST_TCP)

if(ENABLE_RASTA_TLS)
    target_compile_definitions(rasta_transport_test_dtls PUBLIC TEST_UDP ENABLE_TLS)
    target_compile_definitions(rasta_transport_test_tls PUBLIC TEST_TCP ENABLE_TLS)

    # The TLS/DTLS tests load their certificates from ../examples (relative to
    # this directory). Generate them as a test fixture so that the tests do not
    # depend on the rcat examples having been run before.
    add_executable(generate_test_certificates
        rasta_transport_test/c/generate_certificates.c
        ../examples/rcat/c/wolfssl_certificate_helper.c
    )
    target_include_directories(generate_test_certificates PRIVATE ../examples/rcat/c)
    target_link_libraries(generate_test_certificates wolfssl)

    set(TEST_CERT_DIR ${CMAKE_CURRENT_BINARY_DIR}/../examples)
    file(MAKE_DIRECTORY ${TEST_CERT_DIR})
    add_test(NAME generate_test_certificates
             COMMAND generate_test_certificates
                     ${TEST_CERT_DIR}/root-ca.pem
                     ${TEST_CERT_DIR}/server.pem
                     ${TEST_CERT_DIR}/server.key)
    set_tests_properties(generate_test_certificates PROPERTIES FIXTURES_SETUP tls_certificates)
    set_tests_properties(test_rasta_transport_test_dtls test_rasta_transport_test_tls
                         PROPERTIES FIXTURES_REQUIRED tls_certificates)
endif(ENABLE_RASTA_TLS)
