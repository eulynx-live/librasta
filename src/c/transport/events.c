#include "events.h"

#include <inttypes.h>
#include <stdlib.h>

#include <rasta/rasta.h>

#include "../experimental/handlers.h"
#include "../logging.h"
#include "../rasta_connection.h"
#include "../rastahandle.h"
#include "../redundancy/rastaredundancy.h"
#include "../retransmission/messages.h"
#include "../retransmission/protocol.h"
#include "../retransmission/safety_retransmission.h"
#include "../util/rmemory.h"
#include "diagnostics.h"
#include "transport.h"

int channel_accept_event(void *carry_data, int _fd) {
    UNUSED(_fd);

    struct accept_event_data *data = carry_data;

    logger_log(data->h->mux.logger, LOG_LEVEL_DEBUG, "RaSTA RedMux accept", "Socket ready to accept");

    struct sockaddr_in addr;
    int fd = transport_accept(data->socket, &addr);

    char str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, str, INET_ADDRSTRLEN);

    // Find the suitable transport channel in the mux
    rasta_transport_channel *channel = find_channel_by_ip_address(data->h, addr);

    if (channel != NULL) {
        channel->file_descriptor = fd;
        channel->receive_event.fd = fd;
        channel->tls_config = data->socket->tls_config;
        channel->connected = true;
        channel->associated_socket = data->socket;
#ifdef ENABLE_TLS
        channel->tls_state = RASTA_TLS_CONNECTION_READY;
        channel->ctx = data->socket->ctx;
        channel->ssl = data->socket->ssl;
#endif

        if (channel->receive_event.callback != NULL) {
            enable_fd_event(&channel->receive_event);
        }
    } else {
        logger_log(data->h->mux.logger, LOG_LEVEL_INFO, "RaSTA RedMux accept", "Rejecting connection from unknown peer %s:%u", str, ntohs(addr.sin_port));
        close(fd);
    }

    return 0;
}

int channel_receive_event(void *carry_data, int fd) {
    UNUSED(fd);

    struct receive_event_data *data = carry_data;
    rasta_connection *connection = data->connection;

    unsigned char buffer[MAX_DEFER_QUEUE_MSG_SIZE] = {0};
    struct sockaddr_in sender = {0};

    bool is_dtls_conn_ready_result = is_dtls_conn_ready(data->socket);

    ssize_t len = receive_callback(data, buffer, &sender);

    char str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sender.sin_addr, str, INET_ADDRSTRLEN);

    rasta_transport_channel *transport_channel = data->channel;

    if (transport_channel == NULL) {
        // We will only enter this branch for UDP and DTLS

        // Find the suitable transport channel in the mux
        transport_channel = find_channel_by_ip_address(data->h, sender);

        if (transport_channel == NULL) {
            // Ignore and continue
            logger_log(data->h->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux receive", "Discarding packet from unknown peer %s:%u", str, ntohs(sender.sin_port));
            return 0;
        }

        connection = data->h->rasta_connection;
        transport_channel->file_descriptor = data->socket->file_descriptor;

        // We can regard UDP channels as 'always connected' (no re-dial possible)
        transport_channel->connected = true;
    }

    run_channel_diagnostics(connection->redundancy_channel, transport_channel->id);

    logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux receive", "Channel %d calling receive", transport_channel->id);

    // when performing DTLS accept, len = 0 doesn't signal a broken connection
    if (len <= 0 && !is_dtls_conn_ready_result) {
        // Connection is broken
        transport_channel->connected = false;

        // Disable receive events so this handler doesn't get called endlessly
        if (data->socket != NULL) {
            disable_fd_event(&data->socket->receive_event);
        }

        if (data->channel != NULL) {
            disable_fd_event(&data->channel->receive_event);
        }

        if (connection != NULL) {
            return handle_closed_transport(connection, connection->redundancy_channel);
        }

        // Ignore and continue
        return 0;
    }

    if (receive_packet(connection->redundancy_channel->mux, transport_channel, buffer, len)) {
        // Deliver messages to the upper layer
        return red_f_deliverDeferQueue(connection, connection->redundancy_channel);
    }

    logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux receive thread", "Channel %d receive done",
               transport_channel->id);
    return 0;
}

int event_connection_expired(void *carry_data, int fd) {
    UNUSED(fd);

    struct timed_event_data *data = carry_data;
    struct rasta_heartbeat_handle *h = (struct rasta_heartbeat_handle *)data->handle;
    logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HEARTBEAT", "T_i timer expired");

    struct rasta_connection *connection = data->connection;
    // so check if connection is valid

    if (connection == NULL) {
        logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HEARTBEAT", "connection is unknown");
        return 0;
    }

    if (connection->hb_locked) {
        logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HEARTBEAT", "connection is hb_locked");
        return 0;
    }

    // connection is valid, check current state
    if (connection->current_state == RASTA_CONNECTION_UP || connection->current_state == RASTA_CONNECTION_RETRREQ || connection->current_state == RASTA_CONNECTION_RETRRUN) {

        // fire heartbeat timeout event
        fire_on_heartbeat_timeout(sr_create_notification_result(NULL, connection));

        // T_i expired -> close connection
        sr_close_connection(connection, RASTA_DISC_REASON_TIMEOUT, 0);
        logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HEARTBEAT", "T_i timer expired - \033[91mdisconnected\033[0m");
    }

    disable_timed_event(&connection->send_heartbeat_event);
    disable_timed_event(&connection->timeout_event);
    return 1;
}

int heartbeat_send_event(void *carry_data, int fd) {
    UNUSED(fd);

    struct timed_event_data *data = carry_data;
    struct rasta_heartbeat_handle *h = (struct rasta_heartbeat_handle *)data->handle;

    logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HEARTBEAT", "send Heartbeat");

    struct rasta_connection *connection = data->connection;

    if (connection == NULL || connection->hb_locked) {
        return 0;
    }

    // connection is valid, check current state
    if (connection->current_state == RASTA_CONNECTION_UP || connection->current_state == RASTA_CONNECTION_RETRREQ || connection->current_state == RASTA_CONNECTION_RETRRUN) {
        sendHeartbeat(connection, 0);

        logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA HEARTBEAT", "Heartbeat sent to %d", connection->remote_id);
    }

    return 0;
}

int data_send_event(void *carry_data, int fd) {
    UNUSED(fd);

    rasta_sending_handle *h = carry_data;

    logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA send handler", "send data");

    rasta_connection *con = h->connection;

    unsigned int retransmission_backlog_size = sr_retransmission_queue_item_count(con);
    // Because of this condition, this method does not reliably free up space in the send queue.
    // However, we need to pass on backpressure to the caller...
    if (retransmission_backlog_size < con->config->retransmission.max_retransmission_queue_size) {
        unsigned int retransmission_available_size = con->config->retransmission.max_retransmission_queue_size - retransmission_backlog_size;
        unsigned int send_backlog_size = sr_send_queue_item_count(con);

        // to prevent discarding packets, we can send at most as many packets as can be added to the retransmission queue
        if (retransmission_available_size < send_backlog_size) {
            send_backlog_size = retransmission_available_size;
        }

        if (send_backlog_size > 0) {
            logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA send handler", "Messages waiting to be sent: %d",
                       sr_send_queue_item_count(con));

            struct RastaMessageData app_messages;
            struct RastaByteArray msg;

            if (send_backlog_size >= h->config->max_packet) {
                send_backlog_size = h->config->max_packet;
            }
            allocateRastaMessageData(&app_messages, send_backlog_size);

            logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA send handler",
                       "Sending %d application messages from queue",
                       send_backlog_size);

            for (unsigned int i = 0; i < send_backlog_size; i++) {

                struct RastaByteArray *elem;
                elem = fifo_pop(con->fifo_send);
                logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA send handler",
                           "Adding application message to data packet");

                allocateRastaByteArray(&msg, elem->length);
                msg.bytes = rmemcpy(msg.bytes, elem->bytes, elem->length);
                freeRastaByteArray(elem);
                rfree(elem);
                app_messages.data_array[i] = msg;
            }

            struct RastaPacket data = createDataMessage(con->remote_id, con->my_id, con->sn_t,
                                                        con->cs_t, cur_timestamp(), con->ts_r,
                                                        app_messages, h->hashing_context);

            struct RastaByteArray packet = rastaModuleToBytes(&data, h->hashing_context);

            struct RastaByteArray *to_fifo = rmalloc(sizeof(struct RastaByteArray));
            allocateRastaByteArray(to_fifo, packet.length);
            rmemcpy(to_fifo->bytes, packet.bytes, packet.length);
            if (!fifo_push(con->fifo_retransmission, to_fifo)) {
                logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA send handler", "discarding packet because retransmission queue is full");
            }

            redundancy_mux_send(con->redundancy_channel, &data, con->role);

            logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA send handler", "Sent data packet from queue");

            con->sn_t = data.sequence_number + 1;

            // set last message ts
            reschedule_event(&con->send_heartbeat_event);

            freeRastaMessageData(&app_messages);
            freeRastaByteArray(&packet);
            freeRastaByteArray(&data.data);
            freeRastaByteArray(&data.checksum);
        }
    }

    if (sr_send_queue_item_count(con) == 0) {
        // Disable this event until new data arrives
        disable_timed_event(&h->send_event);
    }

    return 0;
}

int send_timed_key_exchange(void *arg, int fd) {
    UNUSED(fd);

#ifdef ENABLE_OPAQUE
    struct timed_event_data *event_data = (struct timed_event_data *)arg;
    // rasta_receive_handle *handle = (rasta_receive_handle *)event_data->handle;
    send_KexRequest(event_data->connection);
    // call periodically
    reschedule_event(&event_data->connection->rekeying_event);
#else
    // should never be called
    (void)arg;
#endif
    return 0;
}
