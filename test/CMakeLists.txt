if(USE_VCPKG)
  find_package(unofficial-cunit CONFIG REQUIRED)
  set(CUNIT_PACKAGE unofficial::cunit::cunit)
else()
  include(FindPkgConfig)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(CUnit IMPORTED_TARGET cunit)
    set(CUNIT_PACKAGE PkgConfig::CUnit)
  endif(PKG_CONFIG_FOUND)
endif(USE_VCPKG)

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
target_link_libraries(rasta_test rasta_udp ${CUNIT_PACKAGE})
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
    target_link_libraries(rasta_transport_test_${RASTA_VARIANT} rasta_${RASTA_VARIANT} ${CUNIT_PACKAGE})
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
endif(ENABLE_RASTA_TLS)
