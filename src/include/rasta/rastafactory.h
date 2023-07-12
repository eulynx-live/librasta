#pragma once

#ifdef __cplusplus
extern "C" { // only need to export C interface if
             // used by C++ source code
#endif

#include <stdint.h>

#include "key_exchange.h"
#include "logging.h"
#include "rastahashing.h"
#include "rastamodule.h"

/**
 * generic struct for the additional data for Connectionrequest and Connectionresponse
 */
struct RastaConnectionData {
    unsigned short send_max;
    char version[5];
};

/**
 * generic struct for the additional data for DisconnectionRequest
 */
struct RastaDisconnectionData {
    unsigned short details;
    unsigned short reason;
};

/**
 * generic struct for the additional data for Datamessage and Retransmitted Datamessage
 */
struct RastaMessageData {
    unsigned int count;
    struct RastaByteArray *data_array;
};

/**
 * returns the last error from a previously called rasta function
 * note: calling this function will reset the errors to none
 * @return the error
 */
rasta_error_type getRastafactoryLastError();

/**
 * Allocates the memory for the rasta message data array. Note: all entrys need to be allocated seperatly (see allocateRastaByteArray in rastamodule.h)
 * @param data
 * @param count
 */
void allocateRastaMessageData(struct RastaMessageData *data, unsigned int count);

/**
 * Frees the MessageData struct and all entries
 * @param data
 */
void freeRastaMessageData(struct RastaMessageData *data);

/**
 * Creates a connection request package
 * @param receiver_id
 * @param sender_id
 * @param initial_sequence_number
 * @param timestamp
 * @param send_max
 * @param version The protocol version. Should be set to "0303"
 * @param hashing_context configuration of the hashing algorithm used by RaSTA
 * @return the created connection request package
 */
struct RastaPacket createConnectionRequest(uint32_t receiver_id, uint32_t sender_id, uint32_t initial_sequence_number,
                                           uint32_t timestamp, uint16_t send_max,
                                           const unsigned char version[4], rasta_hashing_context_t *hashing_context);

/**
 * Creates a connection response package
 * @param receiver_id
 * @param sender_id
 * @param initial_sequence_number
 * @param confirmed_sequence_number
 * @param timestamp
 * @param confirmed_timestamp
 * @param send_max
 * @param version The protocol version. Should be set to "0303"
 * @param hashing_context configuration of the hashing algorithm used by RaSTA
 * @return the created connection response package
 */
struct RastaPacket createConnectionResponse(uint32_t receiver_id, uint32_t sender_id, uint32_t initial_sequence_number, uint32_t confirmed_sequence_number,
                                            uint32_t timestamp, uint32_t confirmed_timestamp, uint16_t send_max,
                                            const unsigned char version[4], rasta_hashing_context_t *hashing_context);
/**
 * Non-standard. Creates a Kex Exchange Request.
 * @param receiver_id
 * @param sender_id
 * @param sequence_number
 * @param confirmed_sequence_number
 * @param timestamp
 * @param confirmed_timestamp
 * @param hashing_context configuration of the hashing algorithm used by RaSTA
 * @param psk
 * @param kex_state
 * @param logger
 * @return the created connection request package
 */
struct RastaPacket createKexRequest(uint32_t receiver_id, uint32_t sender_id, uint32_t sequence_number, uint32_t confirmed_sequence_number,
                                    uint32_t timestamp, uint32_t confirmed_timestamp, rasta_hashing_context_t *hashing_context, const char *psk, struct key_exchange_state *kex_state, struct logger_t *logger);
/**
 * Non-standard. Creates a Key Exchange Response.
 * @param receiver_id
 * @param sender_id
 * @param sequence_number
 * @param confirmed_sequence_number
 * @param timestamp
 * @param confirmed_timestamp
 * @param hashing_context
 * @param psk
 * @param received_client_kex_request
 * @param client_kex_request_length
 * @param initial_sequence_number
 * @param kex_state
 * @param kex_config
 * @param logger
 * @return
 */
struct RastaPacket createKexResponse(uint32_t receiver_id, uint32_t sender_id, uint32_t sequence_number, uint32_t confirmed_sequence_number,
                                     uint32_t timestamp, uint32_t confirmed_timestamp, rasta_hashing_context_t *hashing_context, const char *psk, const uint8_t *received_client_kex_request, size_t client_kex_request_length, uint32_t initial_sequence_number, struct key_exchange_state *kex_state, const struct RastaConfigKex *kex_config, struct logger_t *logger);
/**
 * Non-standard. Creates a Key Exchange Authentication PDU.
 * @param receiver_id
 * @param sender_id
 * @param sequence_number
 * @param confirmed_sequence_number
 * @param timestamp
 * @param confirmed_timestamp
 * @param hashing_context
 * @param user_authentication
 * @param user_authentication_length
 * @param logger
 * @return
 */
struct RastaPacket createKexAuthentication(uint32_t receiver_id, uint32_t sender_id, uint32_t sequence_number, uint32_t confirmed_sequence_number,
                                           uint32_t timestamp, uint32_t confirmed_timestamp, rasta_hashing_context_t *hashing_context, const uint8_t *user_authentication, size_t user_authentication_length, struct logger_t *logger);
/**
 * Extracts the extra data for connectionrequests(6200) and connectionresponse(6201)
 * @param p
 * @return
 */
struct RastaConnectionData extractRastaConnectionData(struct RastaPacket *p);

/**
 * creates a retransmission request package
 * @param receiver_id
 * @param sender_id
 * @param sequence_number
 * @param confirmed_sequence_number
 * @param timestamp
 * @param confirmed_timestamp
 * @param hashing_context configuration of the hashing algorithm used by RaSTA
 * @return the created retransmission request package
 */
struct RastaPacket createRetransmissionRequest(uint32_t receiver_id, uint32_t sender_id, uint32_t sequence_number, uint32_t confirmed_sequence_number,
                                               uint32_t timestamp, uint32_t confirmed_timestamp, rasta_hashing_context_t *hashing_context);

/**
 * creates a retransmission response package
 * @param receiver_id
 * @param sender_id
 * @param sequence_number
 * @param confirmed_sequence_number
 * @param timestamp
 * @param confirmed_timestamp
 * @param hashing_context configuration of the hashing algorithm used by RaSTA
 * @return the created retransmission response package
 */
struct RastaPacket createRetransmissionResponse(uint32_t receiver_id, uint32_t sender_id, uint32_t sequence_number, uint32_t confirmed_sequence_number,
                                                uint32_t timestamp, uint32_t confirmed_timestamp, rasta_hashing_context_t *hashing_context);

/**
 * creates a disconnection request
 * @param receiver_id
 * @param sender_id
 * @param sequence_number
 * @param confirmed_sequence_number
 * @param timestamp
 * @param confirmed_timestamp
 * @param data defined in 5.4.6
 * @param hashing_context configuration of the hashing algorithm used by RaSTA
 * @return the created disconnection request packet
 */
struct RastaPacket createDisconnectionRequest(uint32_t receiver_id, uint32_t sender_id, uint32_t sequence_number, uint32_t confirmed_sequence_number,
                                              uint32_t timestamp, uint32_t confirmed_timestamp, struct RastaDisconnectionData data, rasta_hashing_context_t *hashing_context);

/**
 * Extracts the extra data for disconnectionrequest(6216)
 * @param p
 * @return
 */
struct RastaDisconnectionData extractRastaDisconnectionData(struct RastaPacket *p);

/**
 * creates a heartbeat package
 * @param receiver_id
 * @param sender_id
 * @param sequence_number
 * @param confirmed_sequence_number
 * @param timestamp
 * @param confirmed_timestamp
 * @param hashing_context configuration of the hashing algorithm used by RaSTA
 * @return the created heartbeat package
 */
struct RastaPacket createHeartbeat(uint32_t receiver_id, uint32_t sender_id, uint32_t sequence_number, uint32_t confirmed_sequence_number,
                                   uint32_t timestamp, uint32_t confirmed_timestamp, rasta_hashing_context_t *hashing_context);

/**
 * creates a data message packet
 * @param receiver_id
 * @param sender_id
 * @param sequence_number
 * @param confirmed_sequence_number
 * @param timestamp
 * @param confirmed_timestamp
 * @param data
 * @param hashing_context configuration of the hashing algorithm used by RaSTA
 * @return the created data message packet
 */
struct RastaPacket createDataMessage(uint32_t receiver_id, uint32_t sender_id, uint32_t sequence_number, uint32_t confirmed_sequence_number,
                                     uint32_t timestamp, uint32_t confirmed_timestamp, struct RastaMessageData data, rasta_hashing_context_t *hashing_context);

/**
 * creates a retransmitted data message packet
 * @param receiver_id
 * @param sender_id
 * @param sequence_number
 * @param confirmed_sequence_number
 * @param timestamp
 * @param confirmed_timestamp
 * @param data
 * @param hashing_context configuration of the hashing algorithm used by RaSTA
 * @return the created retransmitted data message packet
 */
struct RastaPacket createRetransmittedDataMessage(uint32_t receiver_id, uint32_t sender_id, uint32_t sequence_number, uint32_t confirmed_sequence_number,
                                                  uint32_t timestamp, uint32_t confirmed_timestamp, struct RastaMessageData data, rasta_hashing_context_t *hashing_context);

/**
 * extracts the additional data for data message and retransmitted data message
 * @param p
 * @return
 */
struct RastaMessageData extractMessageData(struct RastaPacket *p);

/**
 * creates a redundancy PDU carrying the specified @p inner_data
 * @param sequence_number the sequence number of the PDU
 * @param inner_data the SR-layer packet that is contained in the PDU
 * @param checksum_type the options for the CRC algorithm that will be used to calculate the checksum
 * @param packet the RaSTA redundancy layer PDU to create
 */
void createRedundancyPacket(uint32_t sequence_number, struct RastaPacket *inner_data, struct crc_options checksum_type, struct RastaRedundancyPacket *packet);

#ifdef __cplusplus
}
#endif
