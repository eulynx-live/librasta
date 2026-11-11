#include "safety_retransmission.h"

#include <rasta/rasta.h>

#include "../rasta_connection.h"
#include "../rastahandle.h"
#include "../redundancy/rasta_redundancy_channel.h"
#include "../retransmission/handlers.h"
#include "../transport/events.h"
#include "../transport/transport.h"
#include "../util/rmemory.h"
#include "protocol.h"

void log_main_loop_state(struct rasta_handle *h, event_system *ev_sys, const char *message);

void sr_update_timeout_interval(long confirmed_timestamp, struct rasta_connection *con, rasta_config_sending *cfg) {
    unsigned long t_local = cur_timestamp();
    unsigned long t_rtd = t_local + (1000 / sysconf(_SC_CLK_TCK)) - confirmed_timestamp;
    con->t_i = (uint32_t)(cfg->t_max - t_rtd);

    // update the timeout start time
    reschedule_event(&con->timeout_event);
}

void reset_diagnostic(struct rasta_connection *connection) {
    for (unsigned int i = 0; i < connection->diagnostic_intervals_length; i++) {
        connection->diagnostic_intervals[i].message_count = 0;
        connection->diagnostic_intervals[i].t_alive_message_count = 0;
    }
}

void sr_diagnostic_update(struct rasta_connection *connection, struct RastaPacket *receivedPacket, rasta_config_sending *cfg) {
    unsigned long t_local = cur_timestamp();
    unsigned long t_rtd = t_local + (1000 / sysconf(_SC_CLK_TCK)) - receivedPacket->confirmed_timestamp;
    unsigned long t_alive = t_local - connection->cts_r;
    for (unsigned int i = 0; i < connection->diagnostic_intervals_length; i++) {
        if (connection->diagnostic_intervals[i].interval_start >= t_rtd && connection->diagnostic_intervals[i].interval_end <= t_rtd) {
            // found the sub interval this message can be assigned to
            ++connection->diagnostic_intervals[i].message_count;

            // lies t_alive in interval range, too?
            if (connection->diagnostic_intervals[i].interval_start >= t_alive && connection->diagnostic_intervals[i].interval_end <= t_alive) {
                ++connection->diagnostic_intervals[i].t_alive_message_count;
            }
            break; // we found our interval. Other executions aren't necessary anymore
        }
    }
    ++connection->received_diagnostic_message_count;
    if (connection->received_diagnostic_message_count >= cfg->diag_window) {
        fire_on_diagnostic_notification(sr_create_notification_result(NULL, connection));
        reset_diagnostic(connection);
    }
}

void sr_add_app_messages_to_buffer(struct rasta_connection *con, struct RastaPacket *packet) {
    struct RastaMessageData received_data;
    received_data = extractMessageData(packet);

    logger_log(con->logger, LOG_LEVEL_DEBUG, "RaSTA add to buffer", "received %d application messages", received_data.count);

    for (unsigned int i = 0; i < received_data.count; ++i) {
        if (fifo_full(con->fifo_receive)) {
            logger_log(con->logger, LOG_LEVEL_INFO, "RaSTA add to buffer", "discarding %d application messages because receive queue is full", received_data.count - i);
            break;
        }

        // push into queue
        struct RastaByteArray *to_fifo = rmalloc(sizeof(struct RastaByteArray));
        allocateRastaByteArray(to_fifo, received_data.data_array[i].length);
        rmemcpy(to_fifo->bytes, received_data.data_array[i].bytes, received_data.data_array[i].length);

        if (!fifo_push(con->fifo_receive, to_fifo)) {
            logger_log(con->logger, LOG_LEVEL_INFO, "RaSTA add to buffer", "could not insert message into receive queue because it is full");
        }

        // fire onReceive event
        fire_on_receive(sr_create_notification_result(NULL, con));

        sr_update_timeout_interval(packet->confirmed_timestamp, con, &con->config->sending);
        sr_diagnostic_update(con, packet, &con->config->sending);
    }

    freeRastaMessageData(&received_data);
}

void sr_remove_confirmed_messages(struct rasta_connection *con) {
    // remove confirmed messages from retransmission fifo
    logger_log(con->logger, LOG_LEVEL_DEBUG, "RaSTA remove confirmed", "confirming messages with SN_PDU <= %lu", (long unsigned int)con->cs_r);

    struct RastaByteArray *elem;
    while ((elem = fifo_pop(con->fifo_retransmission)) != NULL) {
        struct RastaPacket packet;
        bytesToRastaPacket(*elem, &con->redundancy_channel->hashing_context, &packet);
        logger_log(con->logger, LOG_LEVEL_DEBUG, "RaSTA remove confirmed", "removing packet with sn = %lu",
                   (long unsigned int)packet.sequence_number);

        freeRastaByteArray(elem);
        freeRastaByteArray(&packet.data);
        freeRastaByteArray(&packet.checksum);
        rfree(elem);

        // message is confirmed when CS_R - SN_PDU >= 0
        // equivalent to SN_PDU <= CS_R
        if (packet.sequence_number == con->cs_r) {
            // this packet has the last same sequence number as the confirmed sn, i.e. the next packet in the queue's
            // SN_PDU will be bigger than CS_R (because of FIFO property of mqueue)
            // that means we removed all confirmed messages and have to leave the loop to stop removing packets
            logger_log(con->logger, LOG_LEVEL_DEBUG, "RaSTA remove confirmed", "last confirmed packet removed");
            break;
        }
    }

    // sending is now possible again (space in the retransmission queue is available), so we should trigger it
    if (fifo_full(con->fifo_send)) {
        data_send_event(&con->send_handle, -1);
    }
}

/* ----- processing of received packet types ----- */

int sr_cts_in_seq(struct rasta_connection *con, rasta_config_sending *cfg, struct RastaPacket *packet) {

    if (packet->type == RASTA_TYPE_HB || packet->type == RASTA_TYPE_DATA || packet->type == RASTA_TYPE_RETRDATA) {
        // Workaround rs 05.04.22
        // what should happen if cts_r is 0 (i.e. no packet received yet)
        if (con->cts_r == 0) {
            return 1;
        }

        // cts_in_seq := 0 <= CTS_PDU - CTS_R < t_i
        if (packet->confirmed_timestamp < con->cts_r) {
            return 0;
        }
        return (packet->confirmed_timestamp - con->cts_r) < cfg->t_max;
    } else {
        // for any other type return always true
        return 1;
    }
}

int sr_sn_in_seq(struct rasta_connection *con, struct RastaPacket *packet) {
    if (packet->type == RASTA_TYPE_CONNREQ || packet->type == RASTA_TYPE_CONNRESP ||
        packet->type == RASTA_TYPE_RETRRESP || packet->type == RASTA_TYPE_DISCREQ) {
        // return always true
        return 1;
    } else {
        // check sn_in_seq := sn_r == sn_pdu
        return (con->sn_r == packet->sequence_number);
    }
}

int sr_sn_range_valid(struct rasta_connection *con, rasta_config_sending *cfg, struct RastaPacket *packet) {
    // for types ConReq, ConResp and RetrResp return true
    if (packet->type == RASTA_TYPE_CONNREQ || packet->type == RASTA_TYPE_CONNRESP || packet->type == RASTA_TYPE_RETRRESP) {
        return 1;
    }

    // else
    // seq. nr. in range when 0 <= SN_PDU - SN_R <= N_SENDMAX * 10
    return ((packet->sequence_number >= con->sn_r) &&
            (packet->sequence_number - con->sn_r) <= (cfg->send_max * 10));
}

int sr_cs_valid(struct rasta_connection *con, struct RastaPacket *packet) {
    if (packet->type == RASTA_TYPE_CONNREQ) {
        // initial CS_PDU has to be 0
        return (packet->confirmed_sequence_number == 0);
    } else if (packet->type == RASTA_TYPE_CONNRESP) {
        // has to be identical to last used (sent) seq. nr.
        return (packet->confirmed_sequence_number == (con->sn_t - 1));
    } else {
        // 0 <= CS_PDU - CS_R < SN_T - CS_R
        return ((packet->confirmed_sequence_number >= con->cs_r) &&
                (packet->confirmed_sequence_number - con->cs_r) < (con->sn_t - con->cs_r));
    }
}

int sr_message_authentic(struct rasta_connection *con, struct RastaPacket *packet) {
    return (packet->sender_id == con->remote_id && packet->receiver_id == con->my_id);
}

int sr_check_packet(struct rasta_connection *con, struct logger_t *logger, rasta_config_sending *cfg, struct RastaPacket *receivedPacket, char *location) {
    // check received packet (5.5.2)
    if (!(receivedPacket->checksum_correct &&
          sr_message_authentic(con, receivedPacket) &&
          sr_sn_range_valid(con, cfg, receivedPacket) &&
          sr_cs_valid(con, receivedPacket) &&
          sr_sn_in_seq(con, receivedPacket) &&
          sr_cts_in_seq(con, cfg, receivedPacket))) {
        // something is invalid -> connection failure
        logger_log(logger, LOG_LEVEL_INFO, location, "received packet invalid");

        logger_log(logger, LOG_LEVEL_DEBUG, location, "checksum = %d", receivedPacket->checksum_correct);
        logger_log(logger, LOG_LEVEL_DEBUG, location, "authentic = %d", sr_message_authentic(con, receivedPacket));
        logger_log(logger, LOG_LEVEL_DEBUG, location, "sn_range_valid = %d", sr_sn_range_valid(con, cfg, receivedPacket));
        logger_log(logger, LOG_LEVEL_DEBUG, location, "cs_valid = %d", sr_cs_valid(con, receivedPacket));
        logger_log(logger, LOG_LEVEL_DEBUG, location, "sn_in_seq = %d", sr_sn_in_seq(con, receivedPacket));
        logger_log(logger, LOG_LEVEL_DEBUG, location, "cts_in_seq = %d", sr_cts_in_seq(con, cfg, receivedPacket));

        return 0;
    }

    return 1;
}

void sr_reset_connection(struct rasta_connection *connection) {
    connection->current_state = RASTA_CONNECTION_CLOSED;
    connection->connected_recv_buffer_size = -1;
    connection->hb_locked = 1;

    disable_timed_event(&connection->send_heartbeat_event);
    disable_timed_event(&connection->timeout_event);

    // set all error counters to 0
    struct rasta_error_counters error_counters;
    error_counters.address = 0;
    error_counters.cs = 0;
    error_counters.safety = 0;
    error_counters.sn = 0;
    error_counters.type = 0;

    connection->errors = error_counters;
}

void sr_close_connection(struct rasta_connection *connection, rasta_disconnect_reason reason, unsigned short details) {
    if (connection->current_state == RASTA_CONNECTION_DOWN || connection->current_state == RASTA_CONNECTION_CLOSED) {
        sr_reset_connection(connection);

        redundancy_channel_close(connection, connection->redundancy_channel);

        // fire connection state changed event
        fire_on_connection_state_change(sr_create_notification_result(NULL, connection));
    } else {
        // need to send DiscReq
        sr_reset_connection(connection);
        sendDisconnectionRequest(connection, reason, details);

        connection->current_state = RASTA_CONNECTION_CLOSED;

        redundancy_channel_close(connection, connection->redundancy_channel);

        // fire connection state changed event
        fire_on_connection_state_change(sr_create_notification_result(NULL, connection));
    }
}

void sr_diagnostic_interval_init(struct rasta_connection *connection, rasta_config_sending *cfg) {
    connection->received_diagnostic_message_count = 0;

    unsigned int diagnostic_interval_length = cfg->t_max / DIAGNOSTIC_INTERVAL_SIZE;
    if (cfg->t_max % DIAGNOSTIC_INTERVAL_SIZE > 0) {
        ++diagnostic_interval_length;
    }
    connection->diagnostic_intervals = rmalloc(diagnostic_interval_length * sizeof(struct diagnostic_interval));
    connection->diagnostic_intervals_length = diagnostic_interval_length;
    for (unsigned int i = 0; i < diagnostic_interval_length; i++) {
        struct diagnostic_interval sub_interval;

        sub_interval.interval_start = DIAGNOSTIC_INTERVAL_SIZE * i;
        // last interval_end could be greater than T_MAX but we don't care. Every connection will be closed when you exceed T_MAX
        sub_interval.interval_end = sub_interval.interval_start + DIAGNOSTIC_INTERVAL_SIZE;
        sub_interval.message_count = 0;
        sub_interval.t_alive_message_count = 0;

        connection->diagnostic_intervals[i] = sub_interval;
    }
}

void sr_init_connection(struct rasta_connection *connection, rasta_role role) {
    sr_reset_connection(connection);
    connection->role = role;

    // initalize diagnostic interval and store it in connection
    // sr_diagnostic_interval_init(connection, cfg);

    // reset last rekeying time
#ifdef ENABLE_OPAQUE
    connection->kex_state.last_key_exchanged_millis = 0;
#endif
}

void sr_retransmit_data(rasta_connection *connection) {

    // prepare Array Buffer
    struct RastaByteArray packets[connection->config->retransmission.max_retransmission_queue_size];

    int buffer_n = 0; // how many valid elements are in the buffer
    buffer_n = fifo_get_size(connection->fifo_retransmission);
    logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA retransmission", "found %d unconfirmed packets", buffer_n);

    // get all packets and store them in the buffer
    for (int j = 0; j < buffer_n; ++j) {
        struct RastaByteArray *element;
        element = fifo_pop(connection->fifo_retransmission);

        allocateRastaByteArray(&packets[j], element->length);
        rmemcpy(packets[j].bytes, element->bytes, element->length);

        freeRastaByteArray(element);
        rfree(element);
    }

    // re-open fifo in write mode
    // now retransmit each packet in the buffer with new sequence numbers
    for (int i = 0; i < buffer_n; i++) {
        logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA retransmission", "retransmit packet %d", i);

        // retrieve retransmission data to
        struct RastaPacket old_p;
        bytesToRastaPacket(packets[i], &connection->redundancy_channel->hashing_context, &old_p);
        logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA retransmission", "convert packet %d to packet structure", i);

        struct RastaMessageData app_messages = extractMessageData(&old_p);
        logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA retransmission", "extract data from packet %d ", i);

        // create new packet for retransmission
        struct RastaPacket data = createRetransmittedDataMessage(connection->remote_id, connection->my_id, connection->sn_t,
                                                                 connection->cs_t, cur_timestamp(), connection->ts_r,
                                                                 app_messages, &connection->redundancy_channel->hashing_context);
        logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA retransmission", "created retransmission packet %d ", i);

        struct RastaByteArray new_p = rastaModuleToBytes(&data, &connection->redundancy_channel->hashing_context);

        // add packet to retrFifo again
        struct RastaByteArray *to_fifo = rmalloc(sizeof(struct RastaByteArray));
        allocateRastaByteArray(to_fifo, new_p.length);
        rmemcpy(to_fifo->bytes, new_p.bytes, new_p.length);
        if (fifo_push(connection->fifo_retransmission, to_fifo)) {
            logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA retransmission", "added packet %d to queue", i);
        } else {
            logger_log(connection->logger, LOG_LEVEL_ERROR, "RaSTA retransmission", "could not add packet to full queue");
        }

        // send packet
        redundancy_mux_send(connection->redundancy_channel, &data, connection->role);
        logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA retransmission", "retransmitted packet with old sn=%lu",
                   (long unsigned int)old_p.sequence_number);

        // increase sn_t
        connection->sn_t = connection->sn_t + 1;

        // set last message ts
        reschedule_event(&connection->send_heartbeat_event);

        // free allocated memory of current packet
        freeRastaByteArray(&packets[i]);
        freeRastaByteArray(&new_p);
        freeRastaByteArray(&old_p.data);
        freeRastaByteArray(&old_p.checksum);
        freeRastaByteArray(&data.data);
        freeRastaByteArray(&data.checksum);

        freeRastaMessageData(&app_messages);
    }

    // close retransmission with heartbeat
    sendHeartbeat(connection, 1);
}

unsigned int sr_retransmission_queue_item_count(struct rasta_connection *connection) {
    return fifo_get_size(connection->fifo_retransmission);
}

unsigned int sr_send_queue_item_count(struct rasta_connection *connection) {
    return fifo_get_size(connection->fifo_send);
}

unsigned int sr_recv_queue_item_count(struct rasta_connection *connection) {
    return fifo_get_size(connection->fifo_receive);
}

void sr_listen(struct rasta_handle *h) {
    redundancy_mux_listen_channels(&h->mux);
}

int sr_send(struct rasta_handle *h, struct rasta_connection *con, struct RastaMessageData app_messages) {
    if (con == NULL)
        return -1;

    if (con->current_state == RASTA_CONNECTION_UP) {
        if (app_messages.count > h->config->sending.max_packet) {
            // too many application messages
            logger_log(h->logger, LOG_LEVEL_ERROR, "RaSTA send", "too many application messages to send in one packet. Maximum is %d",
                       h->config->sending.max_packet);
            // do nothing and leave method with error code
            return -1;
        }

        for (unsigned int i = 0; i < app_messages.count; ++i) {
            struct RastaByteArray msg;
            msg = app_messages.data_array[i];

            // push into queue
            struct RastaByteArray *to_fifo = rmalloc(sizeof(struct RastaByteArray));
            allocateRastaByteArray(to_fifo, msg.length);
            rmemcpy(to_fifo->bytes, msg.bytes, msg.length);

            if (fifo_full(con->fifo_send)) {
                // Flush, send queued messages now
                data_send_event(&con->send_handle, -1);
            }

            if (!fifo_push(con->fifo_send, to_fifo)) {
                logger_log(h->logger, LOG_LEVEL_INFO, "RaSTA send", "could not insert message into send queue");
                return -1;
            } else {
                // Enable timed sending
                enable_timed_event(&con->send_handle.send_event);
                logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA send", "data in send queue");
            }
        }

    } else if (con->current_state == RASTA_CONNECTION_CLOSED || con->current_state == RASTA_CONNECTION_DOWN) {
        // nothing to do besides changing state to closed
        con->current_state = RASTA_CONNECTION_CLOSED;

        // fire connection state changed event
        fire_on_connection_state_change(sr_create_notification_result(h, con));
    } else {
        logger_log(h->logger, LOG_LEVEL_ERROR, "RaSTA send", "service not allowed");

        // disconnect and close
        sendDisconnectionRequest(con, RASTA_DISC_REASON_SERVICENOTALLOWED, 0);
        con->current_state = RASTA_CONNECTION_CLOSED;

        // fire connection state changed event
        fire_on_connection_state_change(sr_create_notification_result(h, con));

        // leave with error code
        return -1;
    }
    return 0;
}

struct rasta_connection *sr_connect(struct rasta_handle *h) {
    rasta_connection *connection = NULL;

    if (h->rasta_connection->remote_id == h->config->general.rasta_id_remote) {
        connection = h->rasta_connection;
    }

    if (connection == NULL || redundancy_channel_connect(&h->mux, connection->redundancy_channel) != 0) {
        return NULL;
    }

    sr_init_connection(connection, RASTA_ROLE_CLIENT);

    // initialize seq nums and timestamps
    connection->sn_t = h->config->initial_sequence_number;

    logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA CONNECT", "Using %lu as initial sequence number",
               (long unsigned int)connection->sn_t);

    connection->cs_t = 0;
    connection->cts_r = cur_timestamp();
    connection->t_i = h->config->sending.t_max;

    unsigned char *version = (unsigned char *)RASTA_VERSION;

    logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA CONNECT", "Local version is %s", version);

    // send ConReq
    struct RastaPacket conreq = createConnectionRequest(connection->remote_id, connection->my_id,
                                                        connection->sn_t, cur_timestamp(),
                                                        h->config->sending.send_max,
                                                        version, &connection->redundancy_channel->hashing_context);
    connection->sn_i = connection->sn_t;

    // Send connection request immediately (don't go through packet batching)
    redundancy_mux_send(connection->redundancy_channel, &conreq, connection->role);

    // increase sequence number
    connection->sn_t++;

    // update state
    connection->current_state = RASTA_CONNECTION_START;

    freeRastaByteArray(&conreq.data);
    freeRastaByteArray(&conreq.checksum);

    // fire connection state changed event
    fire_on_connection_state_change(sr_create_notification_result(NULL, connection));
    // Wait for connection response

    enable_timed_event(&connection->handshake_timeout_event);

    logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA CONNECT", "awaiting connection response from %d", connection->remote_id);
    log_main_loop_state(h, h->ev_sys, "event-system started");
    event_system_start(h->ev_sys);

    disable_timed_event(&connection->handshake_timeout_event);

    // What happened? Timeout, or user abort, or success?
    if (connection->current_state != RASTA_CONNECTION_UP) {
        redundancy_channel_close(connection, connection->redundancy_channel);
        return NULL;
    }

    logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA CONNECT", "handshake completed with %d", connection->remote_id);

    return connection;
}

void sr_disconnect(struct rasta_connection *con) {
    logger_log(con->logger, LOG_LEVEL_INFO, "RaSTA connection", "disconnected %X", con->remote_id);

    sr_close_connection(con, RASTA_DISC_REASON_USERREQUEST, 0);

    disable_timed_event(&con->timeout_event);
    disable_timed_event(&con->send_heartbeat_event);
#ifdef ENABLE_OPAQUE
    if (con->config->kex.rekeying_interval_ms) {
        disable_timed_event(&con->rekeying_event);
    }
#endif
}

void sr_cleanup(struct rasta_handle *h) {
    logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA Cleanup", "Cleanup called");

    // set notification pointers to NULL
    h->notifications.on_receive = NULL;
    h->notifications.on_connection_state_change = NULL;
    h->notifications.on_diagnostic_notification = NULL;
    h->notifications.on_disconnection_request_received = NULL;
    h->notifications.on_redundancy_diagnostic_notification = NULL;

    // close mux
    redundancy_mux_close(&h->mux);

    logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA Cleanup", "Cleanup done");
}

#ifdef ENABLE_OPAQUE
bool sr_rekeying_skipped(struct rasta_connection *connection, struct RastaConfigKex *kexConfig) {
    uint64_t current_time;
    if (connection->current_state == RASTA_CONNECTION_KEX_REQ) {
        // already waiting for key exchange
        return false;
    }

    if (connection->role != RASTA_ROLE_SERVER) {
        // client cannot expect to receive key requests from server
        return false;
    }

    if (!kexConfig->rekeying_interval_ms || !connection->kex_state.last_key_exchanged_millis) {
        // no rekeying or no initial time yet
        return false;
    }

    current_time = get_current_time_ms();

    return current_time - connection->kex_state.last_key_exchanged_millis > REKEYING_ALLOWED_DELAY_MS + kexConfig->rekeying_interval_ms;
}
#else
bool sr_rekeying_skipped(struct rasta_connection *connection, struct RastaConfigKex *kexConfig) {
    // no rekeying possible without key exchange
    (void)connection;
    (void)kexConfig;
    return false;
}
#endif

int sr_receive(rasta_connection *con, struct RastaPacket *receivedPacket) {
    logger_log(con->logger, LOG_LEVEL_DEBUG, "RaSTA RECEIVE", "Received packet %d from %d to %d %u", receivedPacket->type, receivedPacket->sender_id, receivedPacket->receiver_id, receivedPacket->length);

    // new client request
    if (receivedPacket->type == RASTA_TYPE_CONNREQ) {
        handle_conreq(con, receivedPacket);

        freeRastaByteArray(&receivedPacket->data);
        freeRastaByteArray(&receivedPacket->checksum);
        return 0;
    }

    if (con == NULL) {
        logger_log(con->logger, LOG_LEVEL_DEBUG, "RaSTA RECEIVE", "Received packet (%d) from unknown source %d", receivedPacket->type, receivedPacket->sender_id);
        // received packet from unknown source
        // TODO: can these packets be ignored?

        freeRastaByteArray(&receivedPacket->data);
        freeRastaByteArray(&receivedPacket->checksum);
        return 0;
    }

    // handle response
    if (receivedPacket->type == RASTA_TYPE_CONNRESP) {
        handle_conresp(con, receivedPacket);

        freeRastaByteArray(&receivedPacket->data);
        freeRastaByteArray(&receivedPacket->checksum);
        // Break from processing (i.e. sr_connect)
        return 1;
    }

    logger_log(con->logger, LOG_LEVEL_DEBUG, "RaSTA RECEIVE", "Checking packet ...");

    // check message checksum
    if (!receivedPacket->checksum_correct) {
        logger_log(con->logger, LOG_LEVEL_ERROR, "RaSTA RECEIVE", "Received packet checksum incorrect");
        // increase safety error counter
        con->errors.safety++;

        freeRastaByteArray(&receivedPacket->data);
        freeRastaByteArray(&receivedPacket->checksum);
        return 0;
    }

    // check for plausible ids
    if (!sr_message_authentic(con, receivedPacket)) {
        logger_log(con->logger, LOG_LEVEL_ERROR, "RaSTA RECEIVE", "Received packet invalid sender/receiver");
        // increase address error counter
        con->errors.address++;

        freeRastaByteArray(&receivedPacket->data);
        freeRastaByteArray(&receivedPacket->checksum);
        return 0;
    }

    // check sequency number range
    if (!sr_sn_range_valid(con, &con->config->sending, receivedPacket)) {
        logger_log(con->logger, LOG_LEVEL_ERROR, "RaSTA RECEIVE", "Received packet sn range invalid");

        // invalid -> increase error counter and discard packet
        con->errors.sn++;

        freeRastaByteArray(&receivedPacket->data);
        freeRastaByteArray(&receivedPacket->checksum);
        return 0;
    }

    // check confirmed sequence number
    if (!sr_cs_valid(con, receivedPacket)) {
        logger_log(con->logger, LOG_LEVEL_ERROR, "RaSTA RECEIVE", "Received packet cs invalid");

        // invalid -> increase error counter and discard packet
        con->errors.cs++;

        freeRastaByteArray(&receivedPacket->data);
        freeRastaByteArray(&receivedPacket->checksum);
        return 0;
    }

    if (sr_rekeying_skipped(con, &con->config->kex)) {
        logger_log(con->logger, LOG_LEVEL_ERROR, "RaSTA KEX", "Did not receive key exchange request for rekeying in time at %" PRIu64 " - disconnecting!", get_current_time_ms());
        sr_close_connection(con, RASTA_DISC_REASON_TIMEOUT, 0);
        freeRastaByteArray(&receivedPacket->data);
        freeRastaByteArray(&receivedPacket->checksum);
        return 0;
    }

    const int result = handle_received_packet(con, receivedPacket);
    freeRastaByteArray(&receivedPacket->data);
    freeRastaByteArray(&receivedPacket->checksum);
    return result;
}

void sr_closed_connection(rasta_connection *connection, unsigned long id) {
    UNUSED(id);
    // logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA Close", "Closing connection to %lu", id);

    connection->current_state = RASTA_CONNECTION_CLOSED;
    sr_reset_connection(connection);

    // remove redundancy channel
    redundancy_channel_close(connection, connection->redundancy_channel);

    // fire connection state changed event
    // TODO: Provide handle to receiver
    fire_on_connection_state_change(sr_create_notification_result(NULL, connection));
}
