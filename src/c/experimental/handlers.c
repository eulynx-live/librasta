#include "handlers.h"

#include <stdlib.h>

#include <rasta/rasta.h>

#include "../rasta_connection.h"
#include "../retransmission/protocol.h"
#include "../retransmission/safety_retransmission.h"

/**
 * processes a received Key Exchange Request packet
 * @param con the used connection
 * @param packet the received Key Exchange Request packet
 */
int handle_kex_request(struct rasta_connection *connection, struct RastaPacket *receivedPacket) {
    logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: Key Exchange Request", "received Data");

    if (sr_sn_in_seq(connection, receivedPacket)) {
        if (connection->current_state != RASTA_CONNECTION_KEX_REQ && (!connection->config->kex.rekeying_interval_ms || connection->current_state != RASTA_CONNECTION_UP)) {
            // received Key Exchange Request in the wrong phase -> disconnect and close
            sr_close_connection(connection, RASTA_DISC_REASON_UNEXPECTEDTYPE, 0);
        } else {
            // sn_in_seq == true -> check cts_in_seq

            logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: KEX Req", "SN in SEQ");

            if (sr_cts_in_seq(connection, &connection->config->sending, receivedPacket)) {
                if (connection->config->kex.mode == KEY_EXCHANGE_MODE_NONE) {
                    logger_log(connection->logger, LOG_LEVEL_ERROR, "RaSTA HANDLE: KEX Req", "Key exchange request received when not activated!");
                    sr_close_connection(connection, RASTA_DISC_REASON_UNEXPECTEDTYPE, 0);
                    return 0;
                }
#ifdef ENABLE_OPAQUE

                struct RastaPacket response;
                connection->kex_state.last_key_exchanged_millis = get_current_time_ms();

                if (connection->kex_state.last_key_exchanged_millis) {
                    logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: KEX Resp", "Accepted rekeying at %" PRIu64, connection->kex_state.last_key_exchanged_millis);
                }

                logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: KEX Req", "CTS in SEQ");

                logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: KEX Req", "Key exchange request received at %" PRIu64, connection->kex_state.last_key_exchanged_millis);
                // valid Key Exchange request packet received

                response = createKexResponse(connection->remote_id, connection->my_id, connection->sn_t,
                                             receivedPacket->sequence_number, cur_timestamp(),
                                             receivedPacket->timestamp, &connection->redundancy_channel->mux->sr_hashing_context, connection->config->kex.psk,
                                             (uint8_t *)receivedPacket->data.bytes, receivedPacket->data.length, connection->sn_i,
                                             &connection->kex_state,
                                             &connection->config->kex, connection->logger);

                redundancy_mux_send(connection->redundancy_channel, &response, connection->role);
                freeRastaByteArray(&response.data);
                freeRastaByteArray(&response.checksum);

                // set values according to 5.6.2 [3]
                connection->sn_r = receivedPacket->sequence_number + 1;
                connection->sn_t += 1;
                connection->cs_t = receivedPacket->sequence_number;
                connection->cs_r = receivedPacket->confirmed_sequence_number;
                connection->ts_r = receivedPacket->timestamp;
                connection->cts_r = receivedPacket->confirmed_timestamp;
                // con->cts_r = current_timestamp();

                // cs_r updated, remove confirmed messages
                sr_remove_confirmed_messages(connection);

                // wait for client to send auth packet, indicating that on the client's side, the exchange worked
                connection->current_state = RASTA_CONNECTION_KEX_AUTH;

                logger_hexdump(connection->logger, LOG_LEVEL_INFO, connection->kex_state.session_key, sizeof(connection->kex_state.session_key), "Setting hash key to:");

                rasta_set_hash_key_variable(&connection->redundancy_channel->mux->sr_hashing_context, (char *)connection->kex_state.session_key, sizeof(connection->kex_state.session_key));

#else
                logger_log(connection->logger, LOG_LEVEL_ERROR, "RaSTA HANDLE: KEX Req", "Not implemented!");

                abort();
#endif

            } else {
                logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: Kex", "CTS not in SEQ");

                // increase cs error counter
                connection->errors.cs++;

                // send DiscReq and close connection
                sr_close_connection(connection, RASTA_DISC_REASON_PROTOCOLERROR, 0);
            }
        }
    } else {
        // received key exchange request in phase during which I should not have received one -> disconnect and close
        sr_close_connection(connection, RASTA_DISC_REASON_UNEXPECTEDTYPE, 0);
    }
    return 0;
}

/**
 * processes a received Key Exchange Response packet
 * @param con the used connection
 * @param packet the received Key Exchange Response packet
 */
int handle_kex_response(struct rasta_connection *connection, struct RastaPacket *receivedPacket) {
    logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: Key Exchange Response", "received Data");

    if (sr_sn_in_seq(connection, receivedPacket)) {
        if (connection->current_state != RASTA_CONNECTION_KEX_RESP) {
            // received Key Exchange Response in the wrong phase -> disconnect and close
            sr_close_connection(connection, RASTA_DISC_REASON_UNEXPECTEDTYPE, 0);
        } else {
            // sn_in_seq == true -> check cts_in_seq
            logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: KEX Resp", "SN in SEQ");

            if (sr_cts_in_seq(connection, &connection->config->sending, receivedPacket)) {
                if (connection->config->kex.mode == KEY_EXCHANGE_MODE_NONE) {
                    logger_log(connection->logger, LOG_LEVEL_ERROR, "RaSTA HANDLE: KEX Resp", "Key exchange response received when not activated!");
                    sr_close_connection(connection, RASTA_DISC_REASON_UNEXPECTEDTYPE, 0);
                    return 0;
                }
#ifdef ENABLE_OPAQUE

                struct RastaPacket response;
                int ret;

                logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: KEX Resp", "CTS in SEQ");
                // valid Key Exchange response packet received
                ret = kex_recover_credential(&connection->kex_state, (const uint8_t *)receivedPacket->data.bytes, receivedPacket->data.length, connection->my_id, connection->remote_id, connection->sn_i, connection->logger);

                if (ret) {
                    logger_log(connection->logger, LOG_LEVEL_ERROR, "RaSTA HANDLE: KEX Resp", "Could not recover credentials!");
                    sr_close_connection(connection, RASTA_DISC_REASON_UNEXPECTEDTYPE, 0);
                    return 1;
                }
                response = createKexAuthentication(connection->remote_id, connection->my_id, connection->sn_t, receivedPacket->sequence_number, cur_timestamp(), receivedPacket->timestamp, &connection->redundancy_channel->mux->sr_hashing_context, connection->kex_state.user_auth_server, sizeof(connection->kex_state.user_auth_server), connection->logger);

                redundancy_mux_send(connection->redundancy_channel, &response, connection->role);
                freeRastaByteArray(&response.data);
                freeRastaByteArray(&response.checksum);

                // set values according to 5.6.2 [3]
                connection->sn_r = receivedPacket->sequence_number + 1;
                connection->sn_t += 1;
                connection->cs_t = receivedPacket->sequence_number;
                connection->cs_r = receivedPacket->confirmed_sequence_number;
                connection->ts_r = receivedPacket->timestamp;
                connection->cts_r = receivedPacket->confirmed_timestamp;
                // con->cts_r = current_timestamp();

                // cs_r updated, remove confirmed messages
                sr_remove_confirmed_messages(connection);

                // kex is done from our PoV, can expect data from now
                connection->current_state = RASTA_CONNECTION_UP;

                logger_hexdump(connection->logger, LOG_LEVEL_INFO, connection->kex_state.session_key, sizeof(connection->kex_state.session_key), "Setting hash key to:");

                rasta_set_hash_key_variable(&connection->redundancy_channel->mux->sr_hashing_context, (char *)connection->kex_state.session_key, sizeof(connection->kex_state.session_key));
#else
                logger_log(connection->logger, LOG_LEVEL_ERROR, "RaSTA HANDLE: KEX Resp", "Not implemented!");
                abort();
#endif
                return 1;
            } else {
                logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: Kex", "CTS not in SEQ");

                // increase cs error counter
                connection->errors.cs++;

                // send DiscReq and close connection
                sr_close_connection(connection, RASTA_DISC_REASON_PROTOCOLERROR, 0);
            }
        }
    } else {
        // received key exchange response in phase during which I should not have received one -> disconnect and close
        sr_close_connection(connection, RASTA_DISC_REASON_UNEXPECTEDTYPE, 0);
    }
    return 0;
}

/**
 * processes a received Key Exchange Authentication packet
 * @param con the used connection
 * @param packet the received Key Exchange Authentication packet
 */
int handle_kex_auth(struct rasta_connection *connection, struct RastaPacket *receivedPacket) {
    logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: Key Exchange Authentication", "received Data");

    if (sr_sn_in_seq(connection, receivedPacket)) {
        if (connection->current_state != RASTA_CONNECTION_KEX_AUTH) {
            // received Key Exchange Authentication in the wrong phase -> disconnect and close
            sr_close_connection(connection, RASTA_DISC_REASON_UNEXPECTEDTYPE, 0);
        } else {
            // sn_in_seq == true -> check cts_in_seq

            logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: KEX Auth", "SN in SEQ");

            if (sr_cts_in_seq(connection, &connection->config->sending, receivedPacket)) {

                if (connection->config->kex.mode == KEY_EXCHANGE_MODE_NONE) {
                    logger_log(connection->logger, LOG_LEVEL_ERROR, "RaSTA HANDLE: KEX Auth", "Key exchange Authentication received when not activated!");
                    sr_close_connection(connection, RASTA_DISC_REASON_UNEXPECTEDTYPE, 0);
                    return 0;
                }
#ifdef ENABLE_OPAQUE
                int ret;
                logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: KEX Auth", "CTS in SEQ");
                // valid Key Exchange Authentication packet received
                ret = kex_authenticate_user(&connection->kex_state, (const uint8_t *)receivedPacket->data.bytes, receivedPacket->data.length, connection->logger);

                if (ret) {
                    logger_log(connection->logger, LOG_LEVEL_ERROR, "RaSTA HANDLE: KEX Auth", "Could not authenticate user");
                    sr_close_connection(connection, RASTA_DISC_REASON_UNEXPECTEDTYPE, 0);
                    return 1;
                }

                // set values according to 5.6.2 [3]
                connection->sn_r = receivedPacket->sequence_number + 1;
                connection->cs_t = receivedPacket->sequence_number;
                connection->cs_r = receivedPacket->confirmed_sequence_number;
                connection->ts_r = receivedPacket->timestamp;
                connection->cts_r = receivedPacket->confirmed_timestamp;
                // con->cts_r = current_timestamp();

                // cs_r updated, remove confirmed messages
                sr_remove_confirmed_messages(connection);

                // kex is done from our PoV, can expect data from now
                connection->current_state = RASTA_CONNECTION_UP;
#else
                logger_log(connection->logger, LOG_LEVEL_ERROR, "RaSTA HANDLE: KEX Auth", "Not implemented!");
                abort();
#endif
                return 1;
            } else {
                logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA HANDLE: Kex", "CTS not in SEQ");

                // increase cs error counter
                connection->errors.cs++;

                // send DiscReq and close connection
                sr_close_connection(connection, RASTA_DISC_REASON_PROTOCOLERROR, 0);
            }
        }
    } else {
        // received key exchange response in phase during which I should not have received one -> disconnect and close
        sr_close_connection(connection, RASTA_DISC_REASON_UNEXPECTEDTYPE, 0);
    }
    return 0;
}

void send_KexRequest(struct rasta_connection *connection) {
#ifdef ENABLE_OPAQUE
    struct RastaPacket hb = createKexRequest(connection->remote_id, connection->my_id, connection->sn_t,
                                             connection->cs_t, cur_timestamp(), connection->ts_r,
                                             &connection->redundancy_channel->mux->sr_hashing_context, connection->config->kex.psk, &connection->kex_state, connection->logger);

    if (!connection->kex_state.last_key_exchanged_millis && connection->config->kex.rekeying_interval_ms) {
        // first key exchanged - need to enable periodic rekeying
        init_send_key_exchange_event(&connection->rekeying_event, &connection->rekeying_carry_data, connection);
        // TODO: Register event (somewhere else)
        enable_timed_event(&connection->rekeying_event);
    } else {
        logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA KEX", "Rekeying at %" PRIu64, get_current_time_ms());
    }

    redundancy_mux_send(connection->redundancy_channel, &hb, connection->role);
    freeRastaByteArray(&hb.data);
    freeRastaByteArray(&hb.checksum);

    connection->sn_t = connection->sn_t + 1;

    connection->kex_state.last_key_exchanged_millis = get_current_time_ms();
    connection->current_state = RASTA_CONNECTION_KEX_RESP;
#else
    // should never be called
    (void)connection;
    abort();
#endif
}
