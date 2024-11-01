#include "tcp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h> //memset
#include <unistd.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfio.h>

#include "../util/rmemory.h"
#include "bsd_utils.h"
#include "ssl_utils.h"
#include "transport.h"

#ifdef WOLFSSL_SET_TLS13_SECRET_CB_EXISTS
/* Callback function for TLS v1.3 secrets for use with Wireshark */
static int Tls13SecretCallback(WOLFSSL *ssl, int id, const unsigned char *secret,
                               int secretSz, void *ctx) {
    int i;
    const char *str = NULL;
    unsigned char clientRandom[32];
    int clientRandomSz;
    XFILE fp = stderr;
    if (ctx) {
        fp = XFOPEN((const char *)ctx, "ab");
        if (fp == XBADFILE) {
            return BAD_FUNC_ARG;
        }
    }

    clientRandomSz = (int)wolfSSL_get_client_random(ssl, clientRandom,
                                                    sizeof(clientRandom));

    if (clientRandomSz <= 0) {
        printf("Error getting client random %d\n", clientRandomSz);
    }

#if 0
    printf("TLS Client Secret CB: Rand %d, Secret %d\n",
        clientRandomSz, secretSz);
#endif

    switch (id) {
    case CLIENT_EARLY_TRAFFIC_SECRET:
        str = "CLIENT_EARLY_TRAFFIC_SECRET";
        break;
    case EARLY_EXPORTER_SECRET:
        str = "EARLY_EXPORTER_SECRET";
        break;
    case CLIENT_HANDSHAKE_TRAFFIC_SECRET:
        str = "CLIENT_HANDSHAKE_TRAFFIC_SECRET";
        break;
    case SERVER_HANDSHAKE_TRAFFIC_SECRET:
        str = "SERVER_HANDSHAKE_TRAFFIC_SECRET";
        break;
    case CLIENT_TRAFFIC_SECRET:
        str = "CLIENT_TRAFFIC_SECRET_0";
        break;
    case SERVER_TRAFFIC_SECRET:
        str = "SERVER_TRAFFIC_SECRET_0";
        break;
    case EXPORTER_SECRET:
        str = "EXPORTER_SECRET";
        break;
    default:
        break;
    }

    if (str != NULL) {
        fprintf(fp, "%s ", str);
    }
    for (i = 0; i < clientRandomSz; i++) {
        fprintf(fp, "%02x", clientRandom[i]);
    }
    fprintf(fp, " ");
    for (i = 0; i < secretSz; i++) {
        fprintf(fp, "%02x", secret[i]);
    }
    fprintf(fp, "\n");

    if (fp != stderr) {
        XFCLOSE(fp);
    }

    return 0;
}
#endif

void tcp_listen(rasta_transport_socket *transport_socket) {
    if (listen(transport_socket->file_descriptor, MAX_PENDING_CONNECTIONS) < 0) {
        // listen failed
        fprintf(stderr, "error when listening to file_descriptor %d", transport_socket->file_descriptor);
        abort();
    }

    wolfssl_start_tls_server(transport_socket, transport_socket->tls_config);
}

int tcp_accept(rasta_transport_socket *socket) {
    struct sockaddr_in empty_sockaddr_in;
    socklen_t sender_len = sizeof(empty_sockaddr_in);
    int socket_fd;
    if ((socket_fd = accept(socket->file_descriptor, (struct sockaddr *)&empty_sockaddr_in, &sender_len)) < 0) {
        perror("tcp failed to accept connection");
        abort();
    }

    /* Create a WOLFSSL object */
    if ((socket->ssl = wolfSSL_new(socket->ctx)) == NULL) {
        fprintf(stderr, "ERROR: failed to create WOLFSSL object\n");
        abort();
    }

    /* Attach wolfSSL to the socket */
    wolfSSL_set_fd(socket->ssl, socket_fd);
    socket->tls_state = RASTA_TLS_CONNECTION_READY;

    /* Establish TLS connection */
    int ret = wolfSSL_accept(socket->ssl);
    if (ret != WOLFSSL_SUCCESS) {
        fprintf(stderr, "wolfSSL_accept error = %d\n",
                wolfSSL_get_error(socket->ssl, ret));
        abort();
    }

    tls_pin_certificate(socket->ssl, socket->tls_config->peer_tls_cert_path);

    set_tls_async(socket_fd, socket->ssl);

    return socket_fd;
}

int tcp_connect(rasta_transport_channel *channel) {
    struct sockaddr_in server;

    rmemset((char *)&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(channel->remote_port);

    // convert host string to usable format
    if (inet_aton(channel->remote_ip_address, &server.sin_addr) == 0) {
        fprintf(stderr, "inet_aton() failed\n");
        abort();
    }

    if (connect(channel->file_descriptor, (struct sockaddr *)&server, sizeof(server)) < 0) {
        channel->connected = false;
        return 1;
    }

    if (channel->ctx == NULL) {
        wolfssl_start_tls_client(channel, channel->tls_config);
    }

    channel->ssl = wolfSSL_new(channel->ctx);
    if (!channel->ssl) {
        const char *error_str = wolfSSL_ERR_reason_error_string(wolfSSL_get_error(channel->ssl, 0));
        fprintf(stderr, "Error allocating WolfSSL session: %s.\n", error_str);
        return 1;
    }

    if (channel->tls_config->tls_hostname[0]) {
        int ret = wolfSSL_check_domain_name(channel->ssl, channel->tls_config->tls_hostname);
        if (ret != SSL_SUCCESS) {
            fprintf(stderr, "Could not add domain name check for domain %s: %d", channel->tls_config->tls_hostname, ret);
            return 1;
        }
    } else {
        fprintf(stderr, "No TLS hostname specified. Will accept ANY valid TLS certificate. Double-check configuration file.\n");
    }
    /* Attach wolfSSL to the socket */
    if (wolfSSL_set_fd(channel->ssl, channel->file_descriptor) != WOLFSSL_SUCCESS) {
        fprintf(stderr, "ERROR: Failed to set the file descriptor\n");
        return 1;
    }

    /* required for getting random used */
    wolfSSL_KeepArrays(channel->ssl);
#ifdef WOLFSSL_SET_TLS13_SECRET_CB_EXISTS
    /* optional logging for wireshark */
    char *sslkeylogfile_path = getenv("SSLKEYLOGFILE");
    if (sslkeylogfile_path != NULL) {
        wolfSSL_set_tls13_secret_cb(channel->ssl, Tls13SecretCallback,
                                    sslkeylogfile_path);
    }
#endif

    /* Connect to wolfSSL on the server side */
    if (wolfSSL_connect(channel->ssl) != WOLFSSL_SUCCESS) {
        const char *error_str = wolfSSL_ERR_reason_error_string(wolfSSL_get_error(channel->ssl, 0));
        fprintf(stderr, "ERROR: failed to connect to wolfSSL %s.\n", error_str);
        return 1;
    }

    tls_pin_certificate(channel->ssl, channel->tls_config->peer_tls_cert_path);
    set_tls_async(channel->file_descriptor, channel->ssl);

    channel->connected = true;

    return 0;
}

ssize_t tcp_receive(rasta_transport_channel *transport_channel, unsigned char *received_message, size_t max_buffer_len, struct sockaddr_in *sender) {
    UNUSED(sender);
    return wolfssl_receive_tls(transport_channel->ssl, received_message, max_buffer_len);
}

void tcp_send(rasta_transport_channel *transport_channel, unsigned char *message, size_t message_len) {
    wolfssl_send_tls(transport_channel->ssl, message, message_len);
}

void tcp_close(rasta_transport_channel *transport_channel) {
    wolfssl_cleanup_channel(transport_channel);
    bsd_close(transport_channel->file_descriptor);
}
