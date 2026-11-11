#include "transport_test_tcp.h"
#include "mock_socket.h"

#include <CUnit/Basic.h>
#include <stdlib.h>

#include "../../../src/c/rastahandle.h"
#include "../../src/c/transport/transport.h"

void test_transport_create_socket_should_initialize_accept_event() {
    // Arrange
    event_system event_system = {0};
    struct rasta_handle h;
    h.ev_sys = &event_system;
    rasta_handle_init(&h, NULL, NULL);

    rasta_transport_socket socket = {0};
    rasta_transport_channel channel = {0};
    rasta_config_tls tls_config = {0};

    transport_init(&h, &channel, 0, "127.0.0.1", 4711, &tls_config);

    // Act
    transport_create_socket(&h, &socket, 0, &tls_config);

    // Assert
    CU_ASSERT_PTR_EQUAL(socket.accept_event.callback, channel_accept_event);
    CU_ASSERT_PTR_EQUAL(socket.accept_event.carry_data, &socket.accept_event_data);
    CU_ASSERT_EQUAL(socket.accept_event.fd, socket.file_descriptor);
}

void test_transport_create_socket_should_initialize_accept_event_data() {
    // Arrange
    event_system event_system = {0};
    struct rasta_handle h;
    h.ev_sys = &event_system;
    rasta_handle_init(&h, NULL, NULL);

    rasta_transport_socket socket = {0};
    rasta_transport_channel channel = {0};
    rasta_config_tls tls_config = {0};

    transport_init(&h, &channel, 0, "127.0.0.1", 4711, &tls_config);

    // Act
    transport_create_socket(&h, &socket, 0, &tls_config);

    // Assert
    CU_ASSERT_PTR_EQUAL(socket.accept_event_data.event, &socket.accept_event);
    CU_ASSERT_PTR_EQUAL(socket.accept_event_data.socket, &socket);
    CU_ASSERT_PTR_EQUAL(socket.accept_event_data.h, &h);
}

void test_transport_create_socket_should_add_accept_event_to_event_system() {
    // Arrange
    event_system event_system = {0};
    struct rasta_handle h;
    h.ev_sys = &event_system;
    rasta_handle_init(&h, NULL, NULL);

    rasta_transport_socket socket = {0};
    rasta_transport_channel channel = {0};
    rasta_config_tls tls_config = {0};

    transport_init(&h, &channel, 0, "127.0.0.1", 4711, &tls_config);

    // Act
    transport_create_socket(&h, &socket, 0, &tls_config);

    // Assert
    CU_ASSERT_PTR_EQUAL(event_system.fd_events.last, &socket.accept_event);
}

void test_transport_listen_should_enable_socket_accept_event() {
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
    CU_ASSERT_FALSE(socket.accept_event.enabled);

    // Act
    transport_listen(&socket);

    // Assert
    CU_ASSERT(socket.accept_event.enabled);

    // Cleanup
    transport_close_socket(&socket);
}

void test_transport_connect_should_enable_channel_receive_event() {
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
    CU_ASSERT_FALSE(channel.receive_event.enabled);

    // Act
    CU_ASSERT_EQUAL(transport_connect(&socket, &channel), 0);

    // Assert
    CU_ASSERT(channel.receive_event.enabled);

    // Cleanup
    transport_close_channel(&channel);
}

void test_transport_close_channel_should_set_unconnected() {
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
    transport_connect(&socket, &channel);

    // Assert
    CU_ASSERT(channel.connected);

    // Act
    transport_close_channel(&channel);

    // Assert
    CU_ASSERT_FALSE(channel.connected);
}

void test_transport_close_channel_should_invalidate_fd() {
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
    transport_connect(&socket, &channel);

    // Act
    transport_close_channel(&channel);

    // Assert
    CU_ASSERT_EQUAL(channel.file_descriptor, -1);
}

void test_transport_close_channel_should_disable_channel_receive_event() {
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
    transport_connect(&socket, &channel);

    // Act
    transport_close_channel(&channel);

    // Assert
    CU_ASSERT_FALSE(channel.receive_event.enabled);
}

void test_transport_redial_should_reconnect() {
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

    rasta_ip_data ip_data = {
        .ip = "127.0.0.1",
        .port = 4711};
    struct RastaConfigRedundancyConnections connections;
    connections.count = 1;
    connections.data = &ip_data;
    rasta_config_redundancy red_config;
    red_config.connections = connections;
    rasta_config_info config;
    config.redundancy = red_config;
    h.mux.config = &config;

    transport_init(&h, &channel, 0, "127.0.0.1", 4711, &tls_config);
    transport_create_socket(&h, &socket, 0, &tls_config);
    transport_connect(&socket, &channel);
    transport_close_channel(&channel);

    // Act
    transport_redial(&channel);

    // Assert
    CU_ASSERT(channel.connected);

    // Cleanup
    transport_close_channel(&channel);
}

void test_transport_redial_should_assign_new_fds() {
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

    rasta_ip_data ip_data = {
        .ip = "127.0.0.1",
        .port = 4711};
    struct RastaConfigRedundancyConnections connections;
    connections.count = 1;
    connections.data = &ip_data;
    rasta_config_redundancy red_config;
    red_config.connections = connections;
    rasta_config_info config;
    config.redundancy = red_config;
    h.mux.config = &config;

    transport_init(&h, &channel, 0, "127.0.0.1", 4711, &tls_config);
    transport_create_socket(&h, &socket, 0, &tls_config);
    transport_connect(&socket, &channel);
    transport_close_channel(&channel);

    // Act
    transport_redial(&channel);

    // Assert
    CU_ASSERT_NOT_EQUAL(channel.file_descriptor, -1);
    CU_ASSERT_NOT_EQUAL(socket.file_descriptor, -1);

    // Cleanup
    transport_close_channel(&channel);
}

void test_transport_redial_should_update_event_fds() {
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

    rasta_ip_data ip_data = {
        .ip = "127.0.0.1",
        .port = 4711};
    struct RastaConfigRedundancyConnections connections;
    connections.count = 1;
    connections.data = &ip_data;
    rasta_config_redundancy red_config;
    red_config.connections = connections;
    rasta_config_info config;
    config.redundancy = red_config;
    h.mux.config = &config;

    transport_init(&h, &channel, 0, "127.0.0.1", 4711, &tls_config);
    transport_create_socket(&h, &socket, 0, &tls_config);
    transport_connect(&socket, &channel);
    transport_close_channel(&channel);

    // Act
    transport_redial(&channel);

    // Assert
    CU_ASSERT_EQUAL(socket.accept_event.fd, socket.file_descriptor);
    CU_ASSERT_EQUAL(socket.receive_event.fd, socket.file_descriptor);

    // Cleanup
    transport_close_channel(&channel);
}
