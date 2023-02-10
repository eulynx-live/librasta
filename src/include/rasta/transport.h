#pragma once

typedef struct redundancy_mux redundancy_mux;

#include <rasta/rastautil.h>
#include <rasta/config.h>

/**
 * representation of the transport channel diagnostic data
 */
typedef struct {
    /**
     * time (in ms since 1.1.1970) when the current diagnose window was started
     */
    unsigned long start_time;

    /**
     * amount of missed or late messages (late means more than T_SEQ later than on fastest channel)
     */
    int n_missed;

    /**
     * average delay, as described in 6.6.3.2 (2)
     */
    unsigned long t_drift;

    /**
     * quadratic delay, as described in 6.6.3.2 (2)
     */
    unsigned long t_drift2;

    /**
     * amount of packets that are received within the current diagnose window
     */
    int received_packets;
} rasta_redundancy_diagnostics_data;

/**
 * representation of a RaSTA redundancy layer transport channel
 */
typedef struct rasta_transport_channel {
    int id;

    int connected;

    /**
     * IPv4 address in format a.b.c.d
     */
    char *ip_address;

    enum RastaTLSMode activeMode;

#ifdef USE_TCP
    /**
     * filedescriptor
     * */
    int fd;
#ifdef ENABLE_TLS
    WOLFSSL *ssl;
#endif
#endif

    /**
     * port number
     */
    uint16_t port;

    /**
     * data used for transport channel diagnostics as in 6.6.3.2
     */
    rasta_redundancy_diagnostics_data diagnostics_data;

    void (*send_callback)(redundancy_mux *mux, struct RastaByteArray data_to_send, struct rasta_transport_channel *channel, unsigned int channel_index);
} rasta_transport_channel;

typedef struct rasta_transport_socket {

    int id;

    int file_descriptor;

    enum RastaTLSMode activeMode;

    const struct RastaConfigTLS *tls_config;

#ifdef ENABLE_TLS

    WOLFSSL_CTX *ctx;

    WOLFSSL *ssl;

    enum rasta_tls_connection_state state;
#endif

} rasta_transport_socket;
