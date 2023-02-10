#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rasta/bsd_utils.h>
#include <rasta/event_system.h>
#include <rasta/rasta_red_multiplexer.h>
#include <rasta/rastahandle.h>
#include <rasta/rastaredundancy.h>
#include <rasta/rmemory.h>
#ifdef USE_UDP
#include <rasta/udp.h>
#endif
#ifdef USE_TCP
#include <rasta/tcp.h>
#endif
#include "../transport/transport.h"
#include <rasta/rastautil.h>

/* --- Notifications --- */

/**
 * wrapper for parameter in the diagnose notification thread handler
 */
struct diagnose_notification_parameter_wrapper {
    /**
     * the used redundancy multiplexer
     */
    redundancy_mux *mux;

    /**
     * value of N_diagnose
     */
    int n_diagnose;

    /**
     * current value of N_missed
     */
    int n_missed;

    /**
     * current value of T_drift
     */
    unsigned long t_drift;

    /**
     * current value of T_drift2
     */
    unsigned long t_drift2;

    /**
     * associated id of the redundancy channel this notification origins from
     */
    unsigned long channel_id;
};

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

    logger_log(&w->mux->logger, LOG_LEVEL_DEBUG, "RaSTA Redundancy onNewConnection caller", "calling onNewConnection function");
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
    free(wrapper);

    logger_log(&mux->logger, LOG_LEVEL_DEBUG, "RaSTA Redundancy call onNewConnection", "called onNewConnection");
}

/**
 * the is the function that handles the call of the onDiagnosticsAvailable notification pointer.
 * this runs on the main thread
 * @param wrapper a wrapper that contains the mux and the diagnose data
 * @return unused
 */
void red_on_diagnostic_caller(struct diagnose_notification_parameter_wrapper *w) {
    logger_log(&w->mux->logger, LOG_LEVEL_DEBUG, "RaSTA Redundancy onDiagnostics caller", "calling onDiagnostics function");
    (*w->mux->notifications.on_diagnostics_available)(w->mux, w->n_diagnose, w->n_missed, w->t_drift, w->t_drift2, w->channel_id);

    w->mux->notifications_running = (unsigned short)(w->mux->notifications_running - 1);
}

/**
 * fires the onDiagnoseAvailable event.
 * This implementation will take care if the function pointer is NULL and start a thread to call the notification
 * @param mux the redundancy multiplexer that is used
 * @param n_diagnose the value of N_Diagnose
 * @param n_missed the current value of N_missed
 * @param t_drift the current value of T_drift
 * @param t_drift2 the current value of T_drift2
 * @param id the id of the redundancy channel
 */
void red_call_on_diagnostic(redundancy_mux *mux, int n_diagnose,
                            int n_missed, unsigned long t_drift, unsigned long t_drift2, unsigned long id) {
    if (mux->notifications.on_diagnostics_available == NULL) {
        // notification not set, do nothing
        return;
    }

    mux->notifications_running++;

    struct diagnose_notification_parameter_wrapper wrapper;
    wrapper.mux = mux;
    wrapper.n_diagnose = n_diagnose;
    wrapper.n_missed = n_missed;
    wrapper.t_drift = t_drift;
    wrapper.t_drift2 = t_drift2;
    wrapper.channel_id = id;

    red_on_diagnostic_caller(&wrapper);

    logger_log(&mux->logger, LOG_LEVEL_DEBUG, "RaSTA Redundancy call onDiagnostics", "called onDiagnostics");
}

/* --------------------- */

// TODO: Sort out these method dependencies
int on_readable_event(void *handle);
int receive_packet(struct rasta_receive_handle *h, redundancy_mux *mux, struct receive_event_data *data) {
    unsigned char *buffer = rmalloc(sizeof(unsigned char) * MAX_DEFER_QUEUE_MSG_SIZE);
    struct sockaddr_in sender = {0};

    logger_log(&mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux receive", "Receive called");

    ssize_t len = receive_callback(mux, data, buffer, &sender);

    if (len == 0) {
        return 0;
    }

    if (len < 0) {
        // TODO: Disable receive event, remove this channel from
        // connected_transport_channels since socket is broken
        // If this is a RaSTA client, try to re-establish a connection somewhere else

        // transport_redial(data->channel);
        return 0;
    }

    logger_log(&mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux receive", "channel %d received data len = %lu", data->channel->id, len);

    size_t len_remaining = len;
    size_t read_offset = 0;
    while (len_remaining > 0) {
        uint16_t currentPacketSize = leShortToHost(&buffer[read_offset]);
        struct RastaRedundancyPacket receivedPacket = handle_received_data(mux, buffer + read_offset, currentPacketSize);
        update_redundancy_channels(mux, data, receivedPacket, &sender, redundancy_channel_extension_callback);

        // Check that deferqueue can take new elements before calling red_f_receiveData
        rasta_redundancy_channel *channel = redundancy_mux_get_channel(mux, receivedPacket.data.sender_id);
        if (deferqueue_isfull(&channel->defer_q) || fifo_full(channel->fifo_recv)) {
            on_readable_event(h);
        }

        red_f_receiveData(channel, receivedPacket, data->channel->id);

        len_remaining -= currentPacketSize;
        read_offset += currentPacketSize;
    }
    rfree(buffer);
    return 0;
}

struct RastaRedundancyPacket handle_received_data(redundancy_mux *mux, unsigned char *buffer, ssize_t len) {
    struct RastaByteArray incomingData;
    incomingData.length = (unsigned int)len;
    incomingData.bytes = buffer;

    rasta_hashing_context_t test;
    struct crc_options options;

    test.hash_length = mux->sr_hashing_context.hash_length;
    test.algorithm = mux->sr_hashing_context.algorithm;
    allocateRastaByteArray(&test.key, mux->sr_hashing_context.key.length);
    rmemcpy(test.key.bytes, mux->sr_hashing_context.key.bytes, mux->sr_hashing_context.key.length);
    rmemcpy(&options, &mux->config.redundancy.crc_type, sizeof(mux->config.redundancy.crc_type));

    struct RastaRedundancyPacket receivedPacket = bytesToRastaRedundancyPacket(incomingData,
                                                                               options, &test);

    freeRastaByteArray(&test.key);
    return receivedPacket;
}

void update_redundancy_channels(redundancy_mux *mux, struct receive_event_data *data, struct RastaRedundancyPacket receivedPacket, struct sockaddr_in *sender, RedundancyChannelExtensionFunction extension_callback) {
    rasta_transport_channel connected_channel;
    connected_channel.ip_address = rmalloc(sizeof(char) * 15);
    sockaddr_to_host(*sender, connected_channel.ip_address);
    connected_channel.port = ntohs(sender->sin_port);
    connected_channel.send_callback = send_callback;

    // find associated redundancy channel
    for (unsigned int i = 0; i < mux->channel_count; ++i) {
        if (receivedPacket.data.sender_id == mux->transport_channels[i].associated_id) {
            // found redundancy channel with associated id
            // need to check if redundancy channel already knows ip & port of sender
            rasta_redundancy_channel channel = mux->transport_channels[i];
            if (channel.connected_transport_channel_count < mux->port_count) {
                // not all remote transport channel endpoints discovered

                int is_channel_saved = 0;

                for (unsigned int j = 0; j < channel.connected_transport_channel_count; ++j) {
                    if (channel.transport_channels[j].port == connected_channel.port &&
                        strcmp(connected_channel.ip_address, channel.transport_channels[j].ip_address) == 0) {
                        // channel is already saved
                        is_channel_saved = 1;
                    }
                }

                if (!is_channel_saved) {
                    // channel wasn't saved yet -> add to list
                    mux->transport_channels[i].transport_channels[channel.connected_transport_channel_count].ip_address = connected_channel.ip_address;
                    mux->transport_channels[i].transport_channels[channel.connected_transport_channel_count].port = connected_channel.port;
                    mux->transport_channels[i].transport_channels[channel.connected_transport_channel_count].send_callback = send_callback;

                    extension_callback(&mux->transport_channels[i].transport_channels[channel.connected_transport_channel_count], data);

                    mux->transport_channels[i].connected_transport_channel_count++;

                    logger_log(&mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux receive", "channel %d discovered client transport channel %s:%d for connection to 0x%lX",
                               data->channel->id, connected_channel.ip_address, connected_channel.port, channel.associated_id);
                } else {
                    // temp channel no longer needed -> free memory
                    rfree(connected_channel.ip_address);
                    break;
                }
            }

            // call the receive function of the associated channel
            /*logger_log(&mux->logger, LOG_LEVEL_DEBUG, "MUX", "count=%d", mux->channel_count);
            for (int k = 0; k < mux->channel_count; ++k) {
                logger_log(&mux->logger, LOG_LEVEL_DEBUG, "MUX", "channel %d, id=%0x%lX", i, mux->transport_channels[i].associated_id);
            }*/
            return;
        }
    }

    // no associated channel found -> received message from new partner
    logger_log(&mux->logger, LOG_LEVEL_INFO, "RaSTA RedMux receive", "received pdu from unknown entity with id=0x%lX",
               (long unsigned int)receivedPacket.data.sender_id);
    rasta_redundancy_channel new_channel;
    red_f_init(mux->logger, mux->config, mux->port_count, receivedPacket.data.sender_id, &new_channel);
    new_channel.associated_id = receivedPacket.data.sender_id;
    // add transport channel to redundancy channel
    new_channel.transport_channels[0].ip_address = connected_channel.ip_address;
    new_channel.transport_channels[0].port = connected_channel.port;
    new_channel.transport_channels[0].send_callback = send_callback;

    extension_callback(&new_channel.transport_channels[0], data);

    new_channel.connected_transport_channel_count++;

    new_channel.is_open = 1;

    // reallocate memory for new client
    mux->transport_channels = rrealloc(mux->transport_channels, (mux->channel_count + 1) * sizeof(rasta_redundancy_channel));

    mux->transport_channels[mux->channel_count] = new_channel;
    mux->channel_count++;

    // fire new redundancy channel notification
    red_call_on_new_connection(mux, new_channel.associated_id);
}

fd_event *prepare_receive_event(struct receive_event_data *data) {
    fd_event *evt = rmalloc(sizeof(fd_event));
    struct receive_event_data *channel_event_data = rmalloc(sizeof(struct receive_event_data));

    *channel_event_data = *data;
    channel_event_data->event = evt;
    memset(evt, 0, sizeof(fd_event));
    evt->enabled = 1;
    evt->carry_data = channel_event_data;
    evt->callback = channel_receive_event;
    evt->fd = data->channel->fd;

    return evt;
}

// #ifdef ENABLE_TLS
// fd_event *prepare_tls_accept_event(fd_event *evt, struct rasta_connected_transport_channel_state *connection) {
//     struct receive_event_data *channel_event_data = evt->carry_data;
//     channel_event_data->ssl = connection->ssl;
//     evt->fd = connection->file_descriptor;

//     return evt;
// }

// int channel_accept_event(void *carry_data) {
//     struct rasta_connected_transport_channel_state connection;
//     struct receive_event_data *data = carry_data;

//     connection.tls_config = &data->h->config.tls;

//     logger_log(&data->h->mux.logger, LOG_LEVEL_DEBUG, "RaSTA RedMux accept", "Socket ready to accept");
//     tcp_accept_tls(&data->h->mux.transport_sockets[data->channel_index], &connection);

//     fd_event *evt = prepare_receive_event(data);
//     prepare_tls_accept_event(evt, &connection);

//     add_fd_event(data->h->ev_sys, evt, EV_READABLE);
//     return 0;
// }
// // #else

int channel_accept_event(void *carry_data) {
    struct receive_event_data *data = carry_data;

    logger_log(&data->h->mux.logger, LOG_LEVEL_DEBUG, "RaSTA RedMux accept", "Socket ready to accept");

    rasta_transport_channel channel;
    transport_accept(&data->h->mux.transport_sockets[data->channel->id], &channel);

    // Now, assign this transport_channel to a redundancy_channel or create a new one (maybe: update_redundancy_channels)
    // In the process of doing that, create fd events for the new socket

    // int socket = tcp_accept(&data->h->mux.transport_sockets[data->channel->id]);
    // data->h->mux.transport_sockets[data->channel->id].file_descriptor = socket;
    // fd_event *evt = prepare_receive_event(data);

    // TODO: Leaked event
    // add_fd_event(data->h->ev_sys, evt, EV_READABLE);
    return 0;
}

void run_channel_diagnostics(struct rasta_handle *h, unsigned int channel_count, unsigned int channel_index) {
    for (unsigned int i = 0; i < channel_count; ++i) {
        rasta_redundancy_channel current = h->mux.transport_channels[i];
        int n_diagnose = h->mux.config.redundancy.n_diagnose;

        unsigned long channel_diag_start_time = current.transport_channels[channel_index].diagnostics_data.start_time;

        if (current_ts() - channel_diag_start_time >= (unsigned long)n_diagnose) {
            // increase n_missed by amount of messages that are not received

            // amount of missed packets
            int missed_count = current.diagnostics_packet_buffer.count -
                               current.transport_channels[channel_index].diagnostics_data.received_packets;

            // increase n_missed
            current.transport_channels[channel_index].diagnostics_data.n_missed += missed_count;

            // window finished, fire event
            // fire diagnostic notification
            red_call_on_diagnostic(&h->mux,
                                   h->mux.config.redundancy.n_diagnose,
                                   current.transport_channels[channel_index].diagnostics_data.n_missed,
                                   current.transport_channels[channel_index].diagnostics_data.t_drift,
                                   current.transport_channels[channel_index].diagnostics_data.t_drift2,
                                   current.associated_id);

            // reset values
            current.transport_channels[channel_index].diagnostics_data.n_missed = 0;
            current.transport_channels[channel_index].diagnostics_data.received_packets = 0;
            current.transport_channels[channel_index].diagnostics_data.t_drift = 0;
            current.transport_channels[channel_index].diagnostics_data.t_drift2 = 0;
            current.transport_channels[channel_index].diagnostics_data.start_time = current_ts();

            deferqueue_clear(&current.diagnostics_packet_buffer);
        }
    }
}

int channel_receive_event(void *carry_data) {
    struct receive_event_data *data = carry_data;
    struct rasta_handle *h = data->h;
    unsigned int mux_channel_count = h->mux.channel_count;

    run_channel_diagnostics(h, mux_channel_count, data->channel->id);

    // channel count might have changed due to removal of channels
    mux_channel_count = h->mux.channel_count;

    logger_log(&h->mux.logger, LOG_LEVEL_DEBUG, "RaSTA RedMux receive thread", "Channel %d calling receive",
               data->channel->id);

    int result = receive_packet(h->receive_handle, &h->mux, data);

    logger_log(&h->mux.logger, LOG_LEVEL_DEBUG, "RaSTA RedMux receive thread", "Channel %d receive done",
               data->channel->id);
    return !!result;
}

int channel_timeout_event(void *carry_data) {
    (void)carry_data;
    // TODO: I don't know what exactly this should handle.

    //  Right now, we are (mis-) using this only to timeout waiting for the handshake response.
    return 1;
}

/**
 * initializes the timeout event
 * @param event the event
 * @param t_data the carry data for the first event
 * @param mux the redundancy multiplexer that will contain the channels
 */
void init_channel_timeout_events(timed_event *event, struct timeout_event_data *t_data, struct redundancy_mux *mux, int channel_timeout_ms) {
    memset(event, 0, sizeof(timed_event));
    t_data->mux = mux;
    t_data->event = event;
    event->callback = channel_timeout_event;
    event->carry_data = t_data;
    event->interval = channel_timeout_ms * 1000000lu;
}

/* ----------------------------*/

void redundancy_mux_init_config(redundancy_mux *mux, struct logger_t logger, struct RastaConfigInfo config) {
    mux->logger = logger;
    mux->port_count = config.redundancy.connections.count;
    mux->listen_ports = rmalloc(sizeof(uint16_t) * mux->port_count);
    mux->config = config;
    mux->notifications_running = 0;

    logger_log(&mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux init", "init memory for %d listen ports", mux->port_count);

    // allocate memory for connected channels
    mux->transport_channels = rmalloc(sizeof(rasta_redundancy_channel));
    mux->channel_count = 0;

    // init notifications to NULL
    mux->notifications.on_diagnostics_available = NULL;
    mux->notifications.on_new_connection = NULL;

    // load ports that are specified in config
    if (mux->config.redundancy.connections.count > 0) {
        logger_log(&mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux init", "loading listen from config");

        // init sockets
        mux->transport_sockets = rmalloc(mux->port_count * sizeof(rasta_transport_connection));
        memset(mux->transport_sockets, 0, mux->port_count * sizeof(rasta_transport_connection));
        for (unsigned i = 0; i < mux->port_count; i++) {
            transport_create_socket(&mux->transport_sockets[i], &mux->config.tls);
        }
    }

    logger_log(&mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux init", "initialization done");
}

redundancy_mux redundancy_mux_init_(struct logger_t logger, uint16_t *listen_ports, unsigned int port_count, struct RastaConfigInfo config) {
    redundancy_mux mux;

    mux.logger = logger;
    mux.listen_ports = listen_ports;
    mux.port_count = port_count;
    mux.config = config;
    mux.notifications_running = 0;
    mux.notifications.on_diagnostics_available = NULL;
    mux.notifications.on_new_connection = NULL;
    logger_log(&mux.logger, LOG_LEVEL_DEBUG, "RaSTA RedMux init", "init memory for %d listen ports", port_count);

    return mux;
}

redundancy_mux redundancy_mux_init(struct logger_t logger, uint16_t *listen_ports, unsigned int port_count, struct RastaConfigInfo config) {
    redundancy_mux mux = redundancy_mux_init_(logger, listen_ports, port_count, config);
    mux.transport_sockets = rmalloc(port_count * sizeof(int));

    // logger_log(&mux.logger, LOG_LEVEL_DEBUG, "RaSTA RedMux init", "setting up tcp socket %d/%d", i + 1, port_count);
    // tcp_init(&mux.rasta_tcp_socket_states[i], &config.tls);
    // tcp_bind_device(&mux.rasta_tcp_socket_states[i], mux.listen_ports[i], mux.config.redundancy.connections.data[i].ip);
    return mux;
}

void redundancy_mux_bind(redundancy_mux *mux) {
    for (unsigned i = 0; i < mux->port_count; ++i) {
        const struct RastaIPData *ip_data = &mux->config.redundancy.connections.data[i];
        transport_bind(&mux->transport_sockets[i], ip_data->ip, (uint16_t)ip_data->port);
    }
}

void redundancy_mux_close(redundancy_mux *mux) {
    // close the redundancy channels
    for (unsigned int j = 0; j < mux->channel_count; ++j) {
        logger_log(&mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux close", "cleanup connected channel %d/%d", j + 1, mux->channel_count);
        red_f_cleanup(&mux->transport_channels[j]);
    }
    rfree(mux->transport_channels);

    // Close listening ports
    for (unsigned int i = 0; i < mux->port_count; ++i) {
        logger_log(&mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux close", "closing socket %d/%d", i + 1, mux->port_count);
        bsd_close(mux->transport_sockets[i].file_descriptor);
    }
    mux->port_count = 0;
    rfree(mux->transport_sockets);

    freeRastaByteArray(&mux->sr_hashing_context.key);
}

rasta_redundancy_channel *redundancy_mux_get_channel(redundancy_mux *mux, unsigned long id) {
    // iterate over all known channels
    for (unsigned int i = 0; i < mux->channel_count; ++i) {
        // check if channel id == wanted id
        if (mux->transport_channels[i].associated_id == id) {
            return &mux->transport_channels[i];
        }
    }

    // wanted id is unknown, return NULL
    return NULL;
}

void redundancy_mux_set_config_id(redundancy_mux *mux, unsigned long id) {
    // only set if a channel is available
    if (mux->channel_count > 0) {
        mux->transport_channels[0].associated_id = id;
    }
}

void redundancy_mux_send(redundancy_mux *mux, struct RastaPacket data) {
    logger_log(&mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux send", "sending a data packet to id 0x%lX",
               (long unsigned int)data.receiver_id);

    // get the channel to the remote entity by the data's received_id
    rasta_redundancy_channel *receiver = redundancy_mux_get_channel(mux, data.receiver_id);

    if (receiver == NULL) {
        logger_log(&mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux send", "redundancy channel with id=0x%lX unknown",
                   (long unsigned int)data.receiver_id);
        // not receiver found
        return;
    }
    logger_log(&mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux send", "current seq_tx=%lu", receiver->seq_tx);

    // create packet to send and convert to byte array
    struct RastaRedundancyPacket packet = createRedundancyPacket(receiver->seq_tx, data, mux->config.redundancy.crc_type);
    struct RastaByteArray data_to_send = rastaRedundancyPacketToBytes(packet, &receiver->hashing_context);

    logger_log(&mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux send", "redundancy packet created");

    // increase seq_tx
    receiver->seq_tx = receiver->seq_tx + 1;

    // send on every transport channels
    for (unsigned int i = 0; i < receiver->connected_transport_channel_count; ++i) {
        logger_log(&mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux send", "Sending on transport channel %d/%d",
                   i + 1, receiver->connected_transport_channel_count);

        rasta_transport_channel *channel = &receiver->transport_channels[i];

        if (!channel->connected) {
            logger_log(&mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux send", "Channel is not connected, re-trying %s:%d",
                    channel->ip_address, channel->port);
            // Attempt to connect (maybe previous attempts were unsuccessful)
            // TODO: Only if this is a RaSTA client, otherwise return
            if (transport_redial(channel) != 0) {
                continue;
            }
        }

        channel->send_callback(mux, data_to_send, channel, i);

        logger_log(&mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux send", "Sent data over channel %s:%d",
                channel->ip_address, channel->port);
    }

    freeRastaByteArray(&data_to_send);

    logger_log(&mux->logger, LOG_LEVEL_DEBUG, "RaSTA Red send", "Data sent over all transport channels");
}

int redundancy_try_mux_retrieve(redundancy_mux *mux, unsigned long id, struct RastaPacket *out) {
    // get the channel by id
    rasta_redundancy_channel *target = redundancy_mux_get_channel(mux, id);

    if (target == NULL) {
        logger_log(&mux->logger, LOG_LEVEL_INFO, "RaSTA RedMux retrieve", "entity with id 0x%lX not connected, passing", id);
        return 0;
    }

    struct RastaByteArray *element;

    if (fifo_get_size(target->fifo_recv) == 0) {
        return 0;
    }

    logger_log(&mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux retrieve", "Found element in queue");

    element = fifo_pop(target->fifo_recv);

    struct RastaPacket packet = bytesToRastaPacket(*element, &target->hashing_context);

    freeRastaByteArray(element);
    rfree(element);

    *out = packet;
    return 1;
}

void redundancy_mux_wait_for_notifications(redundancy_mux *mux) {
    if (mux->notifications_running == 0) {
        logger_log(&mux->logger, LOG_LEVEL_INFO, "RaSTA RedMux wait", "all notification threads finished");
        return;
    }
    logger_log(&mux->logger, LOG_LEVEL_INFO, "RaSTA RedMux wait", "waiting for %d notification thread(s) to finish", mux->notifications_running);
    while (mux->notifications_running > 0) {
        // busy wait
        // to avoid to much CPU utilization, force context switch by sleeping for 0ns
        nanosleep((const struct timespec[]){{0, 0L}}, NULL);
    }
    logger_log(&mux->logger, LOG_LEVEL_INFO, "RaSTA RedMux wait", "all notification threads finished");
}

void redundancy_mux_wait_for_entity(redundancy_mux *mux, unsigned long id) {
    logger_log(&mux->logger, LOG_LEVEL_INFO, "RaSTA RedMux wait", "waiting for entity with id=0x%lX", id);
    rasta_redundancy_channel *target = NULL;
    while (target == NULL) {
        target = redundancy_mux_get_channel(mux, id);
        // to avoid too much CPU utilization, force context switch by sleeping for 0ns
        nanosleep((const struct timespec[]){{0, 0L}}, NULL);
    }
    logger_log(&mux->logger, LOG_LEVEL_INFO, "RaSTA RedMux wait", "entity with id=0x%lX available", id);
}

void redundancy_mux_listen_channels(redundancy_mux *mux) {
    for (unsigned i = 0; i < mux->port_count; ++i) {
        transport_listen(&mux->transport_sockets[i]);
    }
}

int rasta_red_add_transport_channel(rasta_redundancy_channel *channel, rasta_transport_socket *transport_state, char *ip, uint16_t port) {
    rasta_transport_channel *transport_connection = &channel->transport_channels[channel->connected_transport_channel_count];
    transport_connect(transport_state, transport_connection, ip, port);
    channel->connected_transport_channel_count++;
    return transport_connection->connected;
}

int redundancy_mux_add_channel(redundancy_mux *mux, unsigned long id, struct RastaIPData *transport_channels, unsigned transport_channels_length) {
    assert(transport_channels_length == mux->port_count);

    rasta_redundancy_channel channel;
    red_f_init(mux->logger, mux->config, mux->port_count, id, &channel);

    // add transport channels
    int success = 0;
    for (unsigned int i = 0; i < mux->port_count; ++i) {
        // Provided transport channels have to match with local ports configured
        success |= rasta_red_add_transport_channel(&channel,
                                        &mux->transport_sockets[i],
                                        transport_channels[i].ip,
                                        (uint16_t)transport_channels[i].port);
    }

    // reallocate memory for new client
    mux->transport_channels = rrealloc(mux->transport_channels, (mux->channel_count + 1) * sizeof(rasta_redundancy_channel));

    mux->transport_channels[mux->channel_count] = channel;
    mux->channel_count++;

    logger_log(&mux->logger, LOG_LEVEL_INFO, "RaSTA RedMux add channel", "added new redundancy channel for ID=0x%lX", id);

    if (success) {
        return 0;
    }
    return 1;
}

void redundancy_mux_remove_channel(redundancy_mux *mux, unsigned long channel_id) {
    rasta_redundancy_channel *channel = redundancy_mux_get_channel(mux, channel_id);
    logger_log(&mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux remove channel", "removing channel with ID=0x%lX", channel_id);

    if (channel == NULL) {
        // no channel with given id
        return;
    }

    rasta_redundancy_channel *new_channels = rmalloc((mux->channel_count - 1) * sizeof(rasta_redundancy_channel));

    int newIndex = 0;
    for (unsigned int i = 0; i < mux->channel_count; ++i) {
        rasta_redundancy_channel c = mux->transport_channels[i];

        if (c.associated_id == channel_id) {
            logger_log(&mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux remove channel", "skipping channel with ID=0x%lX", c.associated_id);
            for (unsigned int i = 0; i < c.connected_transport_channel_count; ++i) {
                rasta_transport_channel *channel = &c.transport_channels[i];
                transport_close(channel);
            }
            // channel to remove, skip
            continue;
        }

        logger_log(&mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux remove channel", "copy channel with ID=0x%lX", c.associated_id);
        // otherwise copy to new channel array
        new_channels[newIndex] = c;
        newIndex++;
    }

    rfree(mux->transport_channels);
    mux->transport_channels = new_channels;
    mux->channel_count = newIndex;
    logger_log(&mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux remove channel", "%d channels left", mux->channel_count);
}

/**
 * gets the amount of messages in the receive queue of the connected channel with index @p redundancy_channel_index
 * @param mux the multiplexer that is used
 * @param redundancy_channel_index the index of the redundancy channel inside the mux transport_channels array
 * @return amount of messages in the queue
 */
unsigned int get_queue_msg_count(redundancy_mux *mux, unsigned int redundancy_channel_index) {
    if (redundancy_channel_index > mux->channel_count - 1) {
        // channel does not exist anymore
        return 0;
    }

    rasta_redundancy_channel channel = mux->transport_channels[redundancy_channel_index];

    if (channel.fifo_recv == NULL) {
        return 0;
    }
    unsigned int size = fifo_get_size(channel.fifo_recv);

    return size;
}

int redundancy_mux_try_retrieve_all(redundancy_mux *mux, struct RastaPacket *out) {
    for (unsigned int i = 0; i < mux->channel_count; i++) {
        unsigned int msg_count = get_queue_msg_count(mux, i);
        if (msg_count > 0) {
            logger_log(&mux->logger, LOG_LEVEL_DEBUG, "RaSTA RedMux retrieve all", "channel with index %d has %u messages", i, msg_count);
            redundancy_try_mux_retrieve(mux, mux->transport_channels[i].associated_id, out);
            return 1;
        }
    }
    return 0;
}
