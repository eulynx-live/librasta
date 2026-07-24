#include "handlers.h"

#include "../experimental/handlers.h"
#include "../rasta_connection.h"
#include "../redundancy/rasta_redundancy_channel.h"
#include "protocol.h"
#include "safety_retransmission.h"

int handle_received_packet(struct rasta_connection *connection, struct RastaPacket *receivedPacket) {
    switch (receivedPacket->type) {
    case RASTA_TYPE_RETRDATA:
        return handle_retrdata(connection, receivedPacket);
    case RASTA_TYPE_DATA:
        return handle_data(connection, receivedPacket);
    case RASTA_TYPE_RETRREQ:
        return handle_retrreq(connection, receivedPacket);
    case RASTA_TYPE_RETRRESP:
        return handle_retrresp(connection, receivedPacket);
    case RASTA_TYPE_DISCREQ:
        return handle_discreq(connection, receivedPacket);
    case RASTA_TYPE_HB:
        return handle_hb(connection, receivedPacket);
#ifdef ENABLE_OPAQUE
    case RASTA_TYPE_KEX_REQUEST:
        return handle_kex_request(connection, receivedPacket);
    case RASTA_TYPE_KEX_RESPONSE:
        return handle_kex_response(connection, receivedPacket);
    case RASTA_TYPE_KEX_AUTHENTICATION:
        return handle_kex_auth(connection, receivedPacket);
#endif
    default:
        logger_log(connection->logger, LOG_LEVEL_ERROR, "RaSTA RECEIVE", "Received unexpected packet type %d", receivedPacket->type);
        // increase type error counter
        connection->errors.type++;
        break;
    }
    return 0;
}

void update_connection_attrs(struct rasta_connection *connection, struct RastaPacket *receivedPacket) {
    connection->sn_r = receivedPacket->sequence_number + 1;
    connection->cs_t = receivedPacket->sequence_number;
    connection->ts_r = receivedPacket->timestamp;
}

void update_confirmed_attrs(struct rasta_connection *connection, struct RastaPacket *receivedPacket) {
    connection->cts_r = receivedPacket->confirmed_timestamp;
    connection->cs_r = receivedPacket->confirmed_sequence_number;
}

int handle_discreq(struct rasta_connection *connection, struct RastaPacket *receivedPacket) {
    logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: DisconnectionRequest", "received DiscReq");

    // 5.6.2 requires updating values according to [1] (CS_T = SN_PDU)
    // But: DiscReq messages are not confirmed and necessarily last. DiscReq message carry relevant CS values.
    connection->cs_r = receivedPacket->confirmed_sequence_number;
    sr_remove_confirmed_messages(connection);

    connection->current_state = RASTA_CONNECTION_CLOSED;
    sr_reset_connection(connection);

    // remove redundancy channel
    redundancy_channel_close(connection, connection->redundancy_channel);

    // fire connection state changed event
    fire_on_connection_state_change(sr_create_notification_result(NULL, connection));
    // fire disconnection request received event
    struct RastaDisconnectionData data = extractRastaDisconnectionData(receivedPacket);
    fire_on_discrequest_state_change(sr_create_notification_result(NULL, connection), data);

    return 1;
}

int handle_data(struct rasta_connection *connection, struct RastaPacket *receivedPacket) {
    logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: Data", "received Data");

    int result = 0;

    if (sr_sn_in_seq(connection, receivedPacket)) {
        if (connection->current_state == RASTA_CONNECTION_START || connection->current_state == RASTA_CONNECTION_KEX_REQ || connection->current_state == RASTA_CONNECTION_KEX_RESP || connection->current_state == RASTA_CONNECTION_KEX_AUTH) {
            // received data in START or when key exchange still in progress-> disconnect and close
            sr_close_connection(connection, RASTA_DISC_REASON_UNEXPECTEDTYPE, 0);
        } else if (connection->current_state == RASTA_CONNECTION_UP) {
            // sn_in_seq == true -> check cts_in_seq

            logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: Data", "SN in SEQ");

            if (sr_cts_in_seq(connection, &connection->config->sending, receivedPacket)) {
                logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: Data", "CTS in SEQ");

                // valid data packet received
                // read application messages and push into queue
                sr_add_app_messages_to_buffer(connection, receivedPacket);
                result = 1;

                // set values according to 5.6.2 [3]
                update_connection_attrs(connection, receivedPacket);
                update_confirmed_attrs(connection, receivedPacket);

                // cs_r updated, remove confirmed messages
                sr_remove_confirmed_messages(connection);

            } else {
                logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: Data", "CTS not in SEQ");

                // increase cs error counter
                connection->errors.cs++;

                // send DiscReq and close connection
                sr_close_connection(connection, RASTA_DISC_REASON_PROTOCOLERROR, 0);
            }
        } else if (connection->current_state == RASTA_CONNECTION_RETRRUN) {
            if (sr_cts_in_seq(connection, &connection->config->sending, receivedPacket)) {
                // set values according to 5.6.2 [3]
                update_connection_attrs(connection, receivedPacket);
            } else {
                logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: Data", "retransmission failed, disconnect and close");
                // retransmission failed, disconnect and close
                sr_close_connection(connection, RASTA_DISC_REASON_PROTOCOLERROR, 0);
            }
        }
    } else {

        if (connection->current_state == RASTA_CONNECTION_START) {
            // received data in START -> disconnect and close
            sr_close_connection(connection, RASTA_DISC_REASON_UNEXPECTEDTYPE, 0);
        } else if (connection->current_state == RASTA_CONNECTION_RETRRUN || connection->current_state == RASTA_CONNECTION_UP) {
            // increase SN error counter
            connection->errors.sn++;

            logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: Data", "send retransmission request");
            // send RetrReq
            sendRetransmissionRequest(connection);

            // change state to RetrReq
            connection->current_state = RASTA_CONNECTION_RETRREQ;

            fire_on_connection_state_change(sr_create_notification_result(NULL, connection));
        } else if (connection->current_state == RASTA_CONNECTION_RETRREQ) {
            logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: Data", "package is ignored - waiting for RETRResponse");
        }
    }

    return result;
}

int handle_retrreq(struct rasta_connection *connection, struct RastaPacket *receivedPacket) {
    logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA receive", "received RetrReq");

    if (sr_sn_in_seq(connection, receivedPacket)) {
        // sn_in_seq == true
        logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA receive", "RetrReq SNinSeq");

        if (connection->current_state == RASTA_CONNECTION_RETRRUN) {
            logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA receive", "RetrReq: got RetrReq packet in RetrRun mode. closing connection.");

            // send DiscReq to client
            sr_close_connection(connection, RASTA_DISC_REASON_RETRFAILED, 0);
            // printf("Connection closed / DiscReq sent!\n");
        }

        // set values according to 5.6.2 [3]
        update_connection_attrs(connection, receivedPacket);
        connection->cs_r = receivedPacket->confirmed_sequence_number;

        // cs_r updated, remove confirmed messages
        sr_remove_confirmed_messages(connection);

        // send retransmission response
        sendRetransmissionResponse(connection);
        logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA receive", "send RetrRes");

        sr_retransmit_data(connection);

        if (connection->current_state == RASTA_CONNECTION_UP) {
            // change state to up
            connection->current_state = RASTA_CONNECTION_UP;
        } else if (connection->current_state == RASTA_CONNECTION_RETRREQ) {
            // change state to RetrReq
            connection->current_state = RASTA_CONNECTION_RETRREQ;
        }

        // fire connection state changed event
        fire_on_connection_state_change(sr_create_notification_result(NULL, connection));
    } else {
        // sn_in_seq == false
        connection->cs_r = receivedPacket->confirmed_sequence_number;

        // cs_r updated, remove confirmed messages
        sr_remove_confirmed_messages(connection);

        // send retransmission response
        sendRetransmissionResponse(connection);
        logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA receive", "send RetrRes");

        sr_retransmit_data(connection);
        // change state to RetrReq
        connection->current_state = RASTA_CONNECTION_RETRREQ;

        // fire connection state changed event
        fire_on_connection_state_change(sr_create_notification_result(NULL, connection));
    }

    return 0;
}

int handle_retrresp(struct rasta_connection *connection, struct RastaPacket *receivedPacket) {
    if (connection->current_state == RASTA_CONNECTION_RETRREQ) {
        logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA receive", "starting receive retransmitted data");
        // check cts_in_seq
        logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA receive", "RetrResp: CTS in Seq");

        // change to retransmission state
        connection->current_state = RASTA_CONNECTION_RETRRUN;

        // set values according to 5.6.2 [3]
        update_connection_attrs(connection, receivedPacket);
        update_confirmed_attrs(connection, receivedPacket);
    } else {
        logger_log(connection->logger, LOG_LEVEL_ERROR, "RaSTA receive", "received packet type retr_resp, but not in state retr_req");
        sr_close_connection(connection, RASTA_DISC_REASON_UNEXPECTEDTYPE, 0);
    }

    return 0;
}

int handle_retrdata(struct rasta_connection *connection, struct RastaPacket *receivedPacket) {
    int result = 0;

    if (sr_sn_in_seq(connection, receivedPacket)) {

        if (connection->current_state == RASTA_CONNECTION_UP) {
            // close connection
            sr_close_connection(connection, RASTA_DISC_REASON_UNEXPECTEDTYPE, 0);
        } else if (connection->current_state == RASTA_CONNECTION_RETRRUN) {
            if (!sr_cts_in_seq(connection, &connection->config->sending, receivedPacket)) {
                // cts not in seq -> close connection
                sr_close_connection(connection, RASTA_DISC_REASON_PROTOCOLERROR, 0);
            } else {
                // cts is in seq -> add data to receive buffer
                logger_log(connection->logger, LOG_LEVEL_DEBUG, "Process RetrData", "CTS in seq, adding messages to buffer");
                sr_add_app_messages_to_buffer(connection, receivedPacket);
                result = 1;

                // set values according to 5.6.2 [3]
                update_connection_attrs(connection, receivedPacket);
                connection->cs_r = receivedPacket->confirmed_sequence_number;
            }
        }
    } else {
        // sn not in seq
        logger_log(connection->logger, LOG_LEVEL_DEBUG, "Process RetrData", "SN not in Seq");
        logger_log(connection->logger, LOG_LEVEL_DEBUG, "Process RetrData", "SN_PDU=%lu, SN_R=%lu",
                   (long unsigned int)receivedPacket->sequence_number, (long unsigned int)connection->sn_r);
        if (connection->current_state == RASTA_CONNECTION_UP) {
            // close connection
            sr_close_connection(connection, RASTA_DISC_REASON_UNEXPECTEDTYPE, 0);
        } else if (connection->current_state == RASTA_CONNECTION_RETRRUN) {
            // send RetrReq
            logger_log(connection->logger, LOG_LEVEL_DEBUG, "Process RetrData", "changing to state RetrReq");
            sendRetransmissionRequest(connection);
            connection->current_state = RASTA_CONNECTION_RETRREQ;
            fire_on_connection_state_change(sr_create_notification_result(NULL, connection));
        }
    }

    return result;
}

int handle_hb(rasta_connection *connection, struct RastaPacket *receivedPacket) {
    logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: Heartbeat", "Received heartbeat from %d", receivedPacket->sender_id);

    if (connection->current_state == RASTA_CONNECTION_START) {
        // heartbeat is for connection setup
        logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: Heartbeat", "Establish connection");

        // if SN not in Seq -> disconnect and close connection
        if (!sr_sn_in_seq(connection, receivedPacket)) {
            logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: Heartbeat", "Connection HB SN not in Seq");

            if (connection->role == RASTA_ROLE_SERVER) {
                // SN not in Seq
                sr_close_connection(connection, RASTA_DISC_REASON_SEQNERROR, 0);
            } else {
                // Client
                sr_close_connection(connection, RASTA_DISC_REASON_UNEXPECTEDTYPE, 0);
            }
        }

        // if client receives HB in START -> disconnect and close
        if (connection->role == RASTA_ROLE_CLIENT) {
            sr_close_connection(connection, RASTA_DISC_REASON_UNEXPECTEDTYPE, 0);
        }

        if (sr_cts_in_seq(connection, &connection->config->sending, receivedPacket)) {
            // set values according to 5.6.2 [3]
            logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: Heartbeat", "Heartbeat is valid connection successful");
            update_connection_attrs(connection, receivedPacket);
            connection->cs_r = receivedPacket->confirmed_sequence_number;

            if (connection->config->kex.mode == KEY_EXCHANGE_MODE_NONE) {
                // sequence number correct, ready to receive data
                connection->current_state = RASTA_CONNECTION_UP;
            } else {
                // need to negotiate session key first
                connection->current_state = RASTA_CONNECTION_KEX_REQ;
            }

            connection->hb_locked = 0;

            // fire connection state changed event
            fire_on_connection_state_change(sr_create_notification_result(NULL, connection));
            // fire handshake complete event
            fire_on_handshake_complete(sr_create_notification_result(NULL, connection));

            // start sending heartbeats
            enable_timed_event(&connection->send_heartbeat_event);

            // arm the timeout timer
            enable_timed_event(&connection->timeout_event);

            connection->is_new = true;

            return 1;
        } else {
            logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: Heartbeat", "Heartbeat is invalid");

            // sequence number check failed -> disconnect
            sr_close_connection(connection, RASTA_DISC_REASON_PROTOCOLERROR, 0);
            return 0;
        }
    }

    if (sr_sn_in_seq(connection, receivedPacket)) {
        logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: Heartbeat", "SN in SEQ");
        // heartbeats also permissible during key exchange phase, since computation could exceed heartbeat interval
        if (connection->current_state == RASTA_CONNECTION_UP || connection->current_state == RASTA_CONNECTION_RETRRUN ||
            connection->current_state == RASTA_CONNECTION_KEX_REQ || connection->current_state == RASTA_CONNECTION_KEX_RESP ||
            connection->current_state == RASTA_CONNECTION_KEX_AUTH) {
            // check cts_in_seq
            if (sr_cts_in_seq(connection, &connection->config->sending, receivedPacket)) {
                logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: Heartbeat", "CTS in SEQ");

                sr_update_timeout_interval(receivedPacket->confirmed_timestamp, connection, &connection->config->sending);
                sr_diagnostic_update(connection, receivedPacket, &connection->config->sending);

                // set values according to 5.6.2 [3]
                update_connection_attrs(connection, receivedPacket);
                update_confirmed_attrs(connection, receivedPacket);

                // cs_r updated, remove confirmed messages
                sr_remove_confirmed_messages(connection);

                if (connection->current_state == RASTA_CONNECTION_RETRRUN) {
                    connection->current_state = RASTA_CONNECTION_UP;
                    logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: Heartbeat", "State changed from RetrRun to Up");
                    fire_on_connection_state_change(sr_create_notification_result(NULL, connection));
                }
            } else {
                logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: Heartbeat", "CTS not in SEQ - send DisconnectionRequest");
                // close connection
                sr_close_connection(connection, RASTA_DISC_REASON_TIMEOUT, 0);
            }
        }
    } else {
        logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: Heartbeat", "SN not in SEQ");

        if (connection->current_state == RASTA_CONNECTION_UP || connection->current_state == RASTA_CONNECTION_RETRRUN) {
            // ignore message, send RetrReq and goto state RetrReq
            // TODO:send retransmission
            // send_retrreq(connection);
            connection->current_state = RASTA_CONNECTION_RETRREQ;
            logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: Heartbeat", "Send retransmission");

            // fire connection state changed event
            fire_on_connection_state_change(sr_create_notification_result(NULL, connection));
        }
    }
    return 0;
}

// HACK
// TODO: Also fill this from kex handlers
void handle_conreq(struct rasta_connection *connection, struct RastaPacket *receivedPacket) {
    logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionRequest", "Received ConnectionRequest from %d", receivedPacket->sender_id);

    if (connection->current_state == RASTA_CONNECTION_CLOSED || connection->current_state == RASTA_CONNECTION_DOWN) {
        sr_init_connection(connection, RASTA_ROLE_SERVER);

        // initialize seq num
        connection->sn_t = connection->sn_i = connection->config->initial_sequence_number;

        logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionRequest", "Using %lu as initial sequence number",
                   (long unsigned int)connection->sn_t);

        connection->current_state = RASTA_CONNECTION_DOWN;

        // check received packet (5.5.2)
        if (!sr_check_packet(connection, connection->logger, &connection->config->sending, receivedPacket, "RaSTA HANDLE: ConnectionRequest")) {
            logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionRequest", "Packet is not valid");
            sr_close_connection(connection, RASTA_DISC_REASON_PROTOCOLERROR, 0);
            return;
        }

        // received packet is a ConReq -> check version
        struct RastaConnectionData connectionData = extractRastaConnectionData(receivedPacket);

        logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionRequest", "Client has version %.4s", connectionData.version);

        if (compare_version(&RASTA_VERSION, &connectionData.version) == 0 ||
            compare_version(&RASTA_VERSION, &connectionData.version) == -1 ||
            version_accepted(connection->config, &connectionData.version)) {

            logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionRequest", "Version accepted");

            // same version, or lower version -> client has to decide -> send ConResp

            // set values according to 5.6.2 [3]
            update_connection_attrs(connection, receivedPacket);
            update_confirmed_attrs(connection, receivedPacket);

            // save N_SENDMAX of partner
            connection->connected_recv_buffer_size = connectionData.send_max;

            connection->t_i = connection->config->sending.t_max;

            unsigned char *version = (unsigned char *)RASTA_VERSION;

            // send ConResp
            struct RastaPacket conresp = createConnectionResponse(connection->remote_id, connection->my_id,
                                                                  connection->sn_t, connection->cs_t,
                                                                  cur_timestamp(), connection->cts_r,
                                                                  connection->config->sending.send_max,
                                                                  version, &connection->redundancy_channel->hashing_context);

            connection->sn_t = connection->sn_t + 1;

            connection->current_state = RASTA_CONNECTION_START;

            fire_on_connection_state_change(sr_create_notification_result(NULL, connection));

            logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionRequest", "Send Connection Response - waiting for Heartbeat");

            // Send connection response immediately (don't go through packet batching)
            redundancy_mux_send(connection->redundancy_channel, &conresp, connection->role);

            freeRastaByteArray(&conresp.data);
        } else {
            logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: ConnectionRequest", "Version unacceptable - sending DisconnectionRequest");
            sr_close_connection(connection, RASTA_DISC_REASON_INCOMPATIBLEVERSION, 0);
        }
    } else {
        logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionRequest", "Connection is in invalid state (%d) send DisconnectionRequest", connection->current_state);
        sr_close_connection(connection, RASTA_DISC_REASON_UNEXPECTEDTYPE, 0);
    }
}

void handle_conresp(struct rasta_connection *connection, struct RastaPacket *receivedPacket) {

    logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionResponse", "Received ConnectionResponse from %d", receivedPacket->sender_id);

    logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionResponse", "Checking packet..");
    if (!sr_check_packet(connection, connection->logger, &connection->config->sending, receivedPacket, "RaSTA HANDLE: ConnectionResponse")) {
        sr_close_connection(connection, RASTA_DISC_REASON_PROTOCOLERROR, 0);
        return;
    }

    if (connection->current_state == RASTA_CONNECTION_START) {
        if (connection->role == RASTA_ROLE_CLIENT) {
            // handle normal conresp
            logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionResponse", "Current state is in order");

            // correct type of packet received -> version check
            struct RastaConnectionData connectionData = extractRastaConnectionData(receivedPacket);

            // logger_log(&connection->logger, LOG_LEVEL_INFO, "RaSTA open connection", "server is running RaSTA version %s", connectionData.version);

            logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionResponse", "Client has version %s", connectionData.version);

            if (version_accepted(connection->config, &connectionData.version)) {

                logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionResponse", "Version accepted");

                // same version or accepted versions -> send hb to complete handshake

                // set values according to 5.6.2 [3]
                update_connection_attrs(connection, receivedPacket);
                connection->cs_r = receivedPacket->confirmed_sequence_number;

                // update state, ready to send data
                connection->current_state = RASTA_CONNECTION_UP;

                // send hb
                logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionResponse", "Sending heartbeat..");
                sendHeartbeat(connection, 1);

#ifdef ENABLE_OPAQUE
                if (connection->config->kex.mode == KEY_EXCHANGE_MODE_OPAQUE) {
                    send_KexRequest(connection);
                }
#endif

                // fire connection state changed event
                fire_on_connection_state_change(sr_create_notification_result(NULL, connection));
                // fire handshake complete event
                fire_on_handshake_complete(sr_create_notification_result(NULL, connection));

                // start sending heartbeats
                enable_timed_event(&connection->send_heartbeat_event);

                connection->hb_locked = 0;

                // save the N_SENDMAX of remote
                connection->connected_recv_buffer_size = connectionData.send_max;

                // arm the timeout timer
                enable_timed_event(&connection->timeout_event);

            } else {
                // version not accepted -> disconnect
                logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionResponse", "Version not acceptable - send DisonnectionRequest");
                sr_close_connection(connection, RASTA_DISC_REASON_INCOMPATIBLEVERSION, 0);
            }
        } else {
            // Server don't receive conresp
            sr_close_connection(connection, RASTA_DISC_REASON_UNEXPECTEDTYPE, 0);

            logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionResponse", "Server received ConnectionResponse - send Disconnection Request");
        }
    } else if (connection->current_state == RASTA_CONNECTION_RETRREQ || connection->current_state == RASTA_CONNECTION_RETRRUN || connection->current_state == RASTA_CONNECTION_UP) {
        sr_close_connection(connection, RASTA_DISC_REASON_UNEXPECTEDTYPE, 0);
        logger_log(connection->logger, LOG_LEVEL_DEBUG, "RaSTA HANDLE: ConnectionResponse", "Received ConnectionResponse in wrong state - semd DisconnectionRequest");
    }
}
