#include "transport_test_udp.h"
#include "mock_socket.h"
#include <CUnit/Basic.h>
#include "../../src/c/transport/transport.h"

void test_transport_create_socket_should_initialize_receive_event() {
    // Arrange
    event_system event_system = {0};
    struct rasta_handle h;
    h.ev_sys = &event_system;
    rasta_handle_init(&h, NULL, NULL);

    rasta_transport_socket socket;
    rasta_transport_channel channel;
    rasta_config_tls tls_config;

    transport_init(&h, &channel, 100, "127.0.0.1", 4711, &tls_config);

    // Act
    transport_create_socket(&h, &socket, 42, &tls_config);

    // Assert
    CU_ASSERT_PTR_EQUAL(socket.receive_event.callback, channel_receive_event);
    CU_ASSERT_PTR_EQUAL(socket.receive_event.carry_data, &socket.receive_event_data);
    CU_ASSERT_EQUAL(socket.receive_event.fd, socket.file_descriptor);
}

void test_transport_create_socket_should_initialize_receive_event_data() {
    // Arrange
    event_system event_system = {0};
    struct rasta_handle h;
    h.ev_sys = &event_system;
    rasta_handle_init(&h, NULL, NULL);

    rasta_transport_socket socket;
    rasta_transport_channel channel;
    rasta_config_tls tls_config;

    transport_init(&h, &channel, 100, "127.0.0.1", 4711, &tls_config);

    // Act
    transport_create_socket(&h, &socket, 42, &tls_config);

    // Assert
    CU_ASSERT_PTR_EQUAL(socket.receive_event_data.socket, &socket);
    CU_ASSERT_PTR_EQUAL(socket.receive_event_data.h, &h);

    // UDP is "connection-less"
    CU_ASSERT_PTR_NULL(socket.receive_event_data.connection);
    CU_ASSERT_PTR_NULL(socket.receive_event_data.channel);

}

void test_transport_create_socket_should_add_receive_event_to_event_system() {
    // Arrange
    event_system event_system = {0};
    struct rasta_handle h;
    h.ev_sys = &event_system;
    rasta_handle_init(&h, NULL, NULL);

    rasta_transport_socket socket;
    rasta_transport_channel channel;
    rasta_config_tls tls_config;

    transport_init(&h, &channel, 100, "127.0.0.1", 4711, &tls_config);

    // Act
    transport_create_socket(&h, &socket, 42, &tls_config);

    // Assert
    CU_ASSERT_PTR_EQUAL(event_system.fd_events.last, &socket.receive_event);
}

void test_transport_listen_should_enable_socket_receive_event() {
    // Arrange
    event_system event_system = {0};
    struct rasta_handle h;
    h.ev_sys = &event_system;
    rasta_handle_init(&h, NULL, NULL);

    rasta_transport_socket socket;
    rasta_transport_channel channel;
    rasta_config_tls tls_config;

    transport_init(&h, &channel, 100, "127.0.0.1", 4711, &tls_config);
    transport_create_socket(&h, &socket, 42, &tls_config);

    // Assert
    CU_ASSERT_FALSE(socket.receive_event.enabled);

    // Act
    transport_listen(&h, &socket);

    // Assert
    CU_ASSERT(socket.receive_event.enabled);
}

void test_transport_connect_should_enable_socket_receive_event() {
    // Arrange
    event_system event_system = {0};
    struct rasta_handle h;
    h.ev_sys = &event_system;
    rasta_handle_init(&h, NULL, NULL);

    rasta_transport_socket socket;
    rasta_transport_channel channel;
    rasta_config_tls tls_config = {
        .mode = TLS_MODE_DISABLED
    };

    transport_init(&h, &channel, 100, "127.0.0.1", 4711, &tls_config);
    transport_create_socket(&h, &socket, 42, &tls_config);

    // Assert
    CU_ASSERT_FALSE(socket.receive_event.enabled);

    // Act
    CU_ASSERT_EQUAL(transport_connect(&socket, &channel, tls_config), 0);

    // Assert
    CU_ASSERT(socket.receive_event.enabled);
}