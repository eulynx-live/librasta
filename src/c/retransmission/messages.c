#include "messages.h"

#include "../rasta_connection.h"
#include "../rastahandle.h"
#include "protocol.h"

/**
 * send a DiscReq to the specified host
 * @param connection the connection which should be used
 * @param reason the reason for the disconnect
 * @param details detailed information about the disconnect
 * @param host the host where the DiscReq will be sent to
 * @param port the port where the DiscReq will be sent to
 */
void sendDisconnectionRequest(struct rasta_connection *connection, rasta_disconnect_reason reason, unsigned short details) {
    struct RastaDisconnectionData disconnectionData;
    disconnectionData.reason = (unsigned short)reason;
    disconnectionData.details = details;

    struct RastaPacket discReq = createDisconnectionRequest(connection->remote_id, connection->my_id,
                                                            connection->sn_t, connection->cs_t,
                                                            cur_timestamp(), connection->ts_r, disconnectionData, &connection->redundancy_channel->mux->sr_hashing_context);

    redundancy_mux_send(connection->redundancy_channel, &discReq, connection->role);

    freeRastaByteArray(&discReq.data);
    freeRastaByteArray(&discReq.checksum);
}

/**
 * send a HB to the specified host
 * @param connection the connection which should be used
 * @param host the host where the HB will be sent to
 * @param port the port where the HB will be sent to
 */
void sendHeartbeat(struct rasta_connection *connection, char reschedule_manually) {
    struct RastaPacket hb = createHeartbeat(connection->remote_id, connection->my_id, connection->sn_t,
                                            connection->cs_t, cur_timestamp(), connection->ts_r, &connection->redundancy_channel->mux->sr_hashing_context);

    redundancy_mux_send(connection->redundancy_channel, &hb, connection->role);
    freeRastaByteArray(&hb.data);
    freeRastaByteArray(&hb.checksum);

    connection->sn_t = connection->sn_t + 1;
    if (reschedule_manually) {
        reschedule_event(&connection->send_heartbeat_event);
    }
}

void sendRetransmissionRequest(struct rasta_connection *connection) {
    struct RastaPacket retrreq = createRetransmissionRequest(connection->remote_id, connection->my_id,
                                                             connection->sn_t, connection->cs_t, cur_timestamp(),
                                                             connection->ts_r, &connection->redundancy_channel->mux->sr_hashing_context);

    redundancy_mux_send(connection->redundancy_channel, &retrreq, connection->role);
    freeRastaByteArray(&retrreq.data);
    freeRastaByteArray(&retrreq.checksum);

    connection->sn_t = connection->sn_t + 1;
}

void sendRetransmissionResponse(struct rasta_connection *connection) {
    struct RastaPacket retrresp = createRetransmissionResponse(connection->remote_id, connection->my_id,
                                                               connection->sn_t, connection->cs_t, cur_timestamp(),
                                                               connection->ts_r, &connection->redundancy_channel->mux->sr_hashing_context);

    redundancy_mux_send(connection->redundancy_channel, &retrresp, connection->role);
    freeRastaByteArray(&retrresp.data);
    freeRastaByteArray(&retrresp.checksum);
    connection->sn_t = connection->sn_t + 1;
}
