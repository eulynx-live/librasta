#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../retransmission/protocol.h"
#include "../retransmission/safety_retransmission.h"
#include "../transport/bsd_utils.h"
#include "../transport/events.h"
#include "../transport/transport.h"
#include "../util/event_system.h"
#include "../util/rastautil.h"
#include "../util/rmemory.h"
#include "rasta_redundancy_channel.h"

/* --- Notifications --- */

/**
 * wrapper for parameter in the onNewNotification notification thread handler
 */
struct new_connection_notification_parameter_wrapper {
    /**
     * the used redundancy multiplexer
     */
    redundancy_mux *mux;

    /**
     * the id of the new redundancy channel
     */
    unsigned long id;
};

/**
 * the is the function that handles the call of the onDiagnosticsAvailable notification pointer.
 * this runs on the main thread
 * @param connection the connection that will be used
 * @return unused
 */
void red_on_new_connection_caller(struct new_connection_notification_parameter_wrapper *w) {

    logger_log(w->mux->logger, LOG_LEVEL_DEBUG, "RaSTA Redundancy onNewConnection caller", "calling onNewConnection function");
    (*w->mux->notifications.on_new_connection)(w->mux, w->id);

    w->mux->notifications_running = (unsigned short)(w->mux->notifications_running - 1);
}

/**
 * fires the onDiagnoseAvailable event.
 * This implementation will take care if the function pointer is NULL and start a thread to call the notification
 * @param mux the redundancy multiplexer that is used
 * @param id the id of the new redundacy channel
 */
void red_call_on_new_connection(redundancy_mux *mux, unsigned long id) {
    if (mux->notifications.on_new_connection == NULL) {
        // notification not set, do nothing
        return;
    }

    mux->notifications_running++;

    struct new_connection_notification_parameter_wrapper *wrapper =
        rmalloc(sizeof(struct new_connection_notification_parameter_wrapper));
    wrapper->mux = mux;
    wrapper->id = id;

    red_on_new_connection_caller(wrapper);
    rfree(wrapper);

    logger_log(mux->logger, LOG_LEVEL_DEBUG, "RaSTA Redundancy call onNewConnection", "called onNewConnection");
}

/* --------------------- */

int receive_packet(redundancy_mux *mux, rasta_transport_channel *transport_channel, unsigned char *buffer, size_t len) {
    int result = 0;
    logger_log(mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux receive", "channel %d received data len = %zu", transport_channel->id, len);

    // minimum size of a redundancy packet on the wire: 8 byte header (length, reserve,
    // seq nr) plus the trailing checksum
    const size_t min_packet_size = 8 + (mux->config->redundancy.crc_type.width / 8);

    size_t len_remaining = len;
    size_t read_offset = 0;
    while (len_remaining > 0) {
        // leShortToHost need at least two more valid bytes
        if (len_remaining < 2) {
            logger_log(mux->logger, LOG_LEVEL_INFO, "RaSTA RedMux receive",
                       "discarding datagram with remaining size %zu",
                       len_remaining);
            break;
        }

        const uint16_t currentPacketSize = leShortToHost(&buffer[read_offset]);

        // The packet size is taken from the wire and must not be trusted blindly:
        // a value of 0 would loop forever and a value exceeding the remaining bytes
        // would underflow len_remaining and read out of bounds.
        if (currentPacketSize < min_packet_size || currentPacketSize > len_remaining) {
            logger_log(mux->logger, LOG_LEVEL_INFO, "RaSTA RedMux receive",
                       "discarding datagram with invalid packet size %u (remaining %zu)",
                       currentPacketSize, len_remaining);
            break;
        }

        struct RastaRedundancyPacket receivedPacket;
        handle_received_data(mux, buffer + read_offset, currentPacketSize, &receivedPacket);
        // Check that deferqueue can take new elements before calling red_f_receiveData
        rasta_redundancy_channel *channel = NULL;
        if (mux->redundancy_channel->associated_id == receivedPacket.data.sender_id) {
            channel = mux->redundancy_channel;
        }
        if (channel == NULL) {
            // Discard incoming packet
            logger_log(mux->logger, LOG_LEVEL_INFO, "RaSTA RedMux receive", "unable to resolve redundancy channel for sender %u", receivedPacket.data.sender_id);
            freeRastaByteArray(&receivedPacket.data.data);
            freeRastaByteArray(&receivedPacket.data.checksum);
        } else if (deferqueue_isfull(&channel->defer_q)) {
            // Discard incoming packet
            logger_log(channel->logger, LOG_LEVEL_INFO, "RaSTA Red receive", "discarding packet because defer queue is full");
            freeRastaByteArray(&receivedPacket.data.data);
            freeRastaByteArray(&receivedPacket.data.checksum);
        } else {
            // If this turned out to be a new connection or new application data, break from processing
            result |= red_f_receiveData(channel, receivedPacket, transport_channel->id);
        }

        len_remaining -= currentPacketSize;
        read_offset += currentPacketSize;
    }

    return result;
}

int handle_closed_transport(rasta_connection *connection, rasta_redundancy_channel *channel) {
    for (unsigned i = 0; i < channel->transport_channel_count; i++) {
        if (channel->transport_channels[i].connected) {
            // Another channel is still connected, continue the event loop
            return 0;
        }
    }

    sr_closed_connection(connection, channel->associated_id);
    return 1;
}

void handle_received_data(redundancy_mux *mux, unsigned char *buffer, ssize_t len, struct RastaRedundancyPacket *receivedPacket) {
    struct RastaByteArray incomingData;
    incomingData.length = (unsigned int)len;
    incomingData.bytes = buffer;

    rasta_hashing_context_t test;
    struct crc_options options;

    test.hash_length = mux->sr_hashing_context.hash_length;
    test.algorithm = mux->sr_hashing_context.algorithm;
    allocateRastaByteArray(&test.key, mux->sr_hashing_context.key.length);
    rmemcpy(test.key.bytes, mux->sr_hashing_context.key.bytes, mux->sr_hashing_context.key.length);
    rmemcpy(&options, &mux->config->redundancy.crc_type, sizeof(mux->config->redundancy.crc_type));

    bytesToRastaRedundancyPacket(incomingData, options, &test, receivedPacket);

    freeRastaByteArray(&test.key);
}

int channel_timeout_event(void *carry_data, int fd) {
    UNUSED(carry_data);
    UNUSED(fd);
    // Escape the event loop
    return 1;
}

/**
 * initializes the timeout event
 * @param event the event
 * @param t_data the carry data for the first event
 * @param mux the redundancy multiplexer that will contain the channels
 */
void init_handshake_timeout_event(timed_event *event, int channel_timeout_ms) {
    memset(event, 0, sizeof(timed_event));
    event->callback = channel_timeout_event;
    event->interval = channel_timeout_ms * NS_PER_MS;
}

/* ----------------------------*/

void redundancy_mux_allocate_channels(struct rasta_handle *h, redundancy_mux *mux, rasta_config_info *config) {
    mux->redundancy_channel = rmalloc(sizeof(rasta_redundancy_channel));

    // load ports that are specified in config
    if (mux->config->redundancy.connections.count > 0) {
        logger_log(mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux init", "loading listen from config");

        // init sockets
        mux->transport_sockets = rmalloc(mux->port_count * sizeof(rasta_transport_socket));
        memset(mux->transport_sockets, 0, mux->port_count * sizeof(rasta_transport_socket));
        for (unsigned i = 0; i < mux->port_count; i++) {
            transport_create_socket(h, &mux->transport_sockets[i], i, &mux->config->tls);
        }
    }

    assert(config->redundancy_remote.connections.count == mux->port_count);
    redundancy_channel_alloc(h, mux->logger, config, mux->redundancy_channel);
    mux->redundancy_channel->mux = mux;
}

void redundancy_mux_alloc(struct rasta_handle *h, redundancy_mux *mux, struct logger_t *logger, rasta_config_info *config) {
    logger_log(logger, LOG_LEVEL_DEBUG, "RaSTA RedMux init", "init memory for %d listen ports", mux->port_count);
    mux->logger = logger;
    mux->port_count = config->redundancy.connections.count;
    mux->listen_ports = rmalloc(sizeof(uint16_t) * mux->port_count);
    mux->config = config;
    mux->notifications_running = 0;

    // init notifications to NULL
    mux->notifications.on_diagnostics_available = NULL;
    mux->notifications.on_new_connection = NULL;

    // init hashing context
    // TODO: Why is this code duplicated in the safety/retransmission layer?
    mux->sr_hashing_context.hash_length = config->sending.md4_type;
    mux->sr_hashing_context.algorithm = config->sending.sr_hash_algorithm;

    if (mux->sr_hashing_context.algorithm == RASTA_ALGO_MD4) {
        // use MD4 IV as key
        rasta_md4_set_key(&mux->sr_hashing_context, config->sending.md4_a, config->sending.md4_b,
                          config->sending.md4_c, config->sending.md4_d);
    } else {
        // use the sr_hash_key
        allocateRastaByteArray(&mux->sr_hashing_context.key, sizeof(unsigned int));

        // convert unsigned in to byte array
        mux->sr_hashing_context.key.bytes[0] = (config->sending.sr_hash_key >> 24) & 0xFF;
        mux->sr_hashing_context.key.bytes[1] = (config->sending.sr_hash_key >> 16) & 0xFF;
        mux->sr_hashing_context.key.bytes[2] = (config->sending.sr_hash_key >> 8) & 0xFF;
        mux->sr_hashing_context.key.bytes[3] = (config->sending.sr_hash_key) & 0xFF;
    }

    redundancy_mux_allocate_channels(h, mux, config);

    logger_log(logger, LOG_LEVEL_DEBUG, "RaSTA RedMux init", "initialization done");
}

bool redundancy_mux_bind(struct rasta_handle *h) {
    bool success = false;
    for (unsigned i = 0; i < h->mux.port_count; ++i) {
        const rasta_ip_data *ip_data = &h->mux.config->redundancy.connections.data[i];
        logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux bind", "binding transport socket %d/%d to %s:%u", i + 1, h->mux.port_count, ip_data->ip, (uint16_t)ip_data->port);
        success |= transport_bind(&h->mux.transport_sockets[i], ip_data->ip, (uint16_t)ip_data->port);
    }
    return success;
}

void redundancy_mux_close(redundancy_mux *mux) {
    // Close listening ports
    for (unsigned int i = 0; i < mux->port_count; ++i) {
        logger_log(mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux close", "closing socket %d/%d", i + 1, mux->port_count);
        transport_close_socket(&mux->transport_sockets[i]);
    }

    mux->port_count = 0;
    rfree(mux->transport_sockets);
    redundancy_channel_free(mux->redundancy_channel);
    rfree(mux->redundancy_channel);
    rfree(mux->listen_ports);

    freeRastaByteArray(&mux->sr_hashing_context.key);
}

void redundancy_mux_send(rasta_redundancy_channel *receiver, struct RastaPacket *data, rasta_role role) {
    redundancy_mux *mux = receiver->mux;
    logger_log(mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux send", "sending a data packet to id 0x%lX",
               (long unsigned int)data->receiver_id);

    logger_log(mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux send", "current seq_tx=%lu", receiver->seq_tx);

    // create packet to send and convert to byte array
    struct RastaRedundancyPacket packet;
    createRedundancyPacket(receiver->seq_tx, data, mux->config->redundancy.crc_type, &packet);
    struct RastaByteArray data_to_send = rastaRedundancyPacketToBytes(&packet, &receiver->hashing_context);

    logger_log(mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux send", "redundancy packet created");

    // increase seq_tx
    receiver->seq_tx = receiver->seq_tx + 1;

    // send on every transport channel
    for (unsigned int i = 0; i < receiver->transport_channel_count; ++i) {
        logger_log(mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux send", "Sending on transport channel %d/%d",
                   i + 1, receiver->transport_channel_count);

        rasta_transport_channel *channel = &receiver->transport_channels[i];

        if (!channel->connected) {
            // Attempt to connect (maybe previous attempts were unsuccessful)
            // only a RaSTA client can initiate reconnect
            if (role == RASTA_ROLE_CLIENT) {
                logger_log(mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux send", "Channel %d/%d is not connected, re-trying %s:%d",
                           i + 1, receiver->transport_channel_count, channel->remote_ip_address, channel->remote_port);
                if (transport_redial(channel) != 0) {
                    continue;
                }
                logger_log(mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux send", "Reconnected channel %d/%d",
                           i + 1, receiver->transport_channel_count);
            } else {
                logger_log(mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux send", "Skipping unconnected channel %d/%d",
                           i + 1, receiver->transport_channel_count);
                continue;
            }
        }

        channel->send_callback(data_to_send, channel);

        logger_log(mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux send", "Sent data over channel %s:%d",
                   channel->remote_ip_address, channel->remote_port);
    }

    freeRastaByteArray(&data_to_send);

    logger_log(mux->logger, LOG_LEVEL_DEBUG, "RaSTA Red send", "Data sent over all transport channels");
}

// TODO: Remove this and next method because it scares me. Only used from tests though.

void redundancy_mux_wait_for_notifications(redundancy_mux *mux) {
    if (mux->notifications_running == 0) {
        logger_log(mux->logger, LOG_LEVEL_INFO, "RaSTA RedMux wait", "all notification threads finished");
        return;
    }
    logger_log(mux->logger, LOG_LEVEL_INFO, "RaSTA RedMux wait", "waiting for %d notification thread(s) to finish", mux->notifications_running);
    while (mux->notifications_running > 0) {
        // busy wait
        // to avoid to much CPU utilization, force context switch by sleeping for 0ns
        nanosleep((const struct timespec[]){{0, 0L}}, NULL);
    }
    logger_log(mux->logger, LOG_LEVEL_INFO, "RaSTA RedMux wait", "all notification threads finished");
}

void redundancy_mux_wait_for_entity(redundancy_mux *mux, unsigned long id) {
    logger_log(mux->logger, LOG_LEVEL_INFO, "RaSTA RedMux wait", "waiting for entity with id=0x%lX", id);
    rasta_redundancy_channel *target = NULL;
    while (target == NULL) {
        if (mux->redundancy_channel->associated_id == id) {
            target = mux->redundancy_channel;
        }
        // to avoid too much CPU utilization, force context switch by sleeping for 0ns
        nanosleep((const struct timespec[]){{0, 0L}}, NULL);
    }
    logger_log(mux->logger, LOG_LEVEL_INFO, "RaSTA RedMux wait", "entity with id=0x%lX available", id);
}

void redundancy_mux_listen_channels(redundancy_mux *mux) {
    for (unsigned i = 0; i < mux->port_count; ++i) {
        transport_listen(&mux->transport_sockets[i]);
    }
}

void redundancy_mux_init(redundancy_mux *mux) {
    redundancy_channel_init(mux->redundancy_channel);
}
