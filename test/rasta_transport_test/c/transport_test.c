#include "transport_test.h"
#include "../../../src/c/rastahandle.h"
#include "../../src/c/transport/transport.h"
#include "mock_socket.h"
#include <CUnit/Basic.h>

void test_transport_init_should_initialize_channel_props() {

    // Arrange
    event_system event_system = {0};
    struct rasta_handle h;
    h.ev_sys = &event_system;
    rasta_handle_init(&h, NULL, NULL);

    rasta_transport_channel channel = {0};
    rasta_config_tls tls_config = {0};

    // Act
    transport_init(&h, &channel, 100, "127.0.0.1", 4711, &tls_config);

    // Assert

    CU_ASSERT_EQUAL(channel.id, 100);
    CU_ASSERT_EQUAL(channel.remote_port, 4711);
    CU_ASSERT_STRING_EQUAL(channel.remote_ip_address, "127.0.0.1");
}

void test_transport_init_should_initialize_receive_event() {

    // Arrange
    event_system event_system = {0};
    struct rasta_handle h;
    h.ev_sys = &event_system;
    rasta_handle_init(&h, NULL, NULL);

    rasta_transport_channel channel = {0};
    rasta_config_tls tls_config = {0};

    // Act
    transport_init(&h, &channel, 0, "127.0.0.1", 4711, &tls_config);

    // Assert

    // Receive event should be set up correctly
    CU_ASSERT_EQUAL(channel.receive_event.carry_data, &channel.receive_event_data);
    CU_ASSERT_EQUAL(channel.receive_event.callback, channel_receive_event);
}

void test_transport_init_should_initialize_receive_event_data() {

    // Arrange
    event_system event_system = {0};
    struct rasta_handle h;
    h.ev_sys = &event_system;
    rasta_handle_init(&h, NULL, NULL);

    rasta_transport_channel channel = {0};
    rasta_config_tls tls_config = {0};

    // Act
    transport_init(&h, &channel, 0, "127.0.0.1", 4711, &tls_config);

    // Assert

    // Receive event data should be set up correctly
    CU_ASSERT_EQUAL(channel.receive_event_data.h, &h);
    CU_ASSERT_EQUAL(channel.receive_event_data.channel, &channel);
    CU_ASSERT_EQUAL(channel.receive_event_data.connection, NULL);
}

void test_transport_init_should_add_receive_event_to_event_system() {

    // Arrange
    event_system event_system = {0};
    struct rasta_handle h;
    h.ev_sys = &event_system;
    rasta_handle_init(&h, NULL, NULL);

    rasta_transport_channel channel = {0};
    rasta_config_tls tls_config = {0};

    // Act
    transport_init(&h, &channel, 0, "127.0.0.1", 4711, &tls_config);

    // Assert

    CU_ASSERT_EQUAL(event_system.fd_events.first, &channel.receive_event);
}

void test_transport_create_socket_should_initialize_socket() {
    // Arrange
    event_system event_system = {0};
    struct rasta_handle h;
    h.ev_sys = &event_system;
    rasta_handle_init(&h, NULL, NULL);

    rasta_transport_socket socket = {0};
    rasta_config_tls tls_config = {0};

    // Act
    transport_create_socket(&h, &socket, 42, &tls_config);

    // Assert
    CU_ASSERT_EQUAL(socket.id, 42);
    CU_ASSERT_PTR_EQUAL(socket.tls_config, &tls_config);
}

void test_transport_create_socket_should_create_fd() {
    // Arrange
    event_system event_system = {0};
    struct rasta_handle h;
    h.ev_sys = &event_system;
    rasta_handle_init(&h, NULL, NULL);

    rasta_transport_socket socket = {0};
    rasta_config_tls tls_config = {0};

    // Act
    transport_create_socket(&h, &socket, 0, &tls_config);

    // Assert
    CU_ASSERT(socket.file_descriptor >= 0);
}

void test_transport_connect_should_set_connected() {
    // Arrange
    event_system event_system = {0};
    struct rasta_handle h;
    h.ev_sys = &event_system;
    rasta_handle_init(&h, NULL, NULL);

    rasta_transport_socket socket = {0};
    rasta_transport_channel channel = {0};
    rasta_config_tls tls_config = {
        .tls_hostname = "localhost",
        .ca_cert_path = "../examples/root-ca.pem",
        .cert_path = "../examples/server.pem",
        .key_path = "../examples/server.key",
    };

    transport_init(&h, &channel, 0, "127.0.0.1", 4711, &tls_config);
    transport_create_socket(&h, &socket, 0, &tls_config);

    // Assert
    CU_ASSERT_FALSE(channel.connected);

    // Act
    CU_ASSERT_EQUAL(transport_connect(&socket, &channel), 0);

    // Assert
    CU_ASSERT(channel.connected);

    // Cleanup
    transport_close_channel(&channel);
}

void test_transport_connect_should_set_equal_fds() {
    // Arrange
    event_system event_system = {0};
    struct rasta_handle h;
    h.ev_sys = &event_system;
    rasta_handle_init(&h, NULL, NULL);

    rasta_transport_socket socket = {0};
    rasta_transport_channel channel = {0};
    rasta_config_tls tls_config = {
        .tls_hostname = "localhost",
        .ca_cert_path = "../examples/root-ca.pem",
        .cert_path = "../examples/server.pem",
        .key_path = "../examples/server.key",
    };

    transport_init(&h, &channel, 0, "127.0.0.1", 4711, &tls_config);
    transport_create_socket(&h, &socket, 0, &tls_config);

    // Act
    CU_ASSERT_EQUAL(transport_connect(&socket, &channel), 0);

    // Assert
    CU_ASSERT_EQUAL(channel.file_descriptor, socket.file_descriptor);

    // Cleanup
    transport_close_channel(&channel);
}
