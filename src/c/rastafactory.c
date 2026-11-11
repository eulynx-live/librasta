#include "rastafactory.h"

#include <stdlib.h>

#include "util/rmemory.h"

/*
 * Public
 */

rasta_error_type rastafactoryLastError = RASTA_ERRORS_NONE;

void allocateRastaMessageData(struct RastaMessageData *data, unsigned int count) {
    data->count = count;
    data->data_array = rmalloc(sizeof(struct RastaByteArray) * count);
}

void freeRastaMessageData(struct RastaMessageData *data) {
    for (unsigned int i = 0; i < data->count; i++) {
        freeRastaByteArray(&data->data_array[i]);
    }
    data->count = 0;
    rfree(data->data_array);
}

rasta_error_type getRastafactoryLastError() {
    rasta_error_type temp = rastafactoryLastError;
    rastafactoryLastError = RASTA_ERRORS_NONE;
    return temp;
}
/**
 * initializes a package, usefull for less lines of code
 * @param type
 * @param receiver_id
 * @param sender_id
 * @param sequence_number
 * @param confirmed_sequence_number
 * @param timestamp
 * @param confirmed_timestamp
 * @param data_length
 * @return
 */
struct RastaPacket initializePacket(rasta_conn_type type, uint32_t receiver_id, uint32_t sender_id, uint32_t sequence_number, uint32_t confirmed_sequence_number,
                                    uint32_t timestamp, uint32_t confirmed_timestamp, uint16_t data_length, rasta_hashing_context_t *hashing_context) {
    struct RastaPacket result;
    result.type = type;
    result.receiver_id = receiver_id;
    result.sender_id = sender_id;
    result.sequence_number = sequence_number;
    result.confirmed_sequence_number = confirmed_sequence_number;
    result.timestamp = timestamp;
    result.confirmed_timestamp = confirmed_timestamp;
    if (data_length > 0) {
        allocateRastaByteArray(&result.data, data_length);
    } else {
        result.data.bytes = NULL;
        result.data.length = 0;
    }

    if (hashing_context->hash_length > 0) {
        allocateRastaByteArray(&result.checksum, hashing_context->hash_length * 8);
    } else {
        result.checksum.bytes = NULL;
        result.checksum.length = 0;
    }

    // calculate the length of the packet
    // static length: 28
    // checksum length: {0,8,16}
    // data length= data_length
    result.length = (uint16_t)(28 + (8 * hashing_context->hash_length) + data_length);

    return result;
}

struct RastaPacket createConnectionRequest(uint32_t receiver_id, uint32_t sender_id, uint32_t initial_sequence_number,
                                           uint32_t timestamp, uint16_t send_max,
                                           const unsigned char version[4], rasta_hashing_context_t *hashing_context) {

    struct RastaPacket p = initializePacket(RASTA_TYPE_CONNREQ, receiver_id, sender_id, initial_sequence_number, 0, timestamp, 0, 14, hashing_context);

    // insert protocol version 03.03. (§10 in ISO)

    p.data.bytes[0] = version[0];
    p.data.bytes[1] = version[1];
    p.data.bytes[2] = version[2];
    p.data.bytes[3] = version[3];

    // insert sendmax

    hostShortTole(send_max, &p.data.bytes[4]);

    // insert zeros

    for (int i = 6; i <= 13; i++) {
        p.data.bytes[i] = 0x00;
    }

    return p;
}

struct RastaPacket createConnectionResponse(uint32_t receiver_id, uint32_t sender_id, uint32_t initial_sequence_number, uint32_t confirmed_sequence_number,
                                            uint32_t timestamp, uint32_t confirmed_timestamp, uint16_t send_max,
                                            const unsigned char version[4], rasta_hashing_context_t *hashing_context) {
    struct RastaPacket p = initializePacket(RASTA_TYPE_CONNRESP, receiver_id, sender_id, initial_sequence_number,
                                            confirmed_sequence_number, timestamp, confirmed_timestamp, 14, hashing_context);

    // insert protocol version 03.03. (§10 in ISO)

    p.data.bytes[0] = version[0];
    p.data.bytes[1] = version[1];
    p.data.bytes[2] = version[2];
    p.data.bytes[3] = version[3];

    // insert sendmax

    hostShortTole(send_max, &p.data.bytes[4]);

    // insert zeros

    for (int i = 6; i <= 13; i++) {
        p.data.bytes[i] = 0x00;
    }

    return p;
}

#ifdef ENABLE_OPAQUE
struct RastaPacket createKexRequest(uint32_t receiver_id, uint32_t sender_id, uint32_t sequence_number, uint32_t confirmed_sequence_number,
                                    uint32_t timestamp, uint32_t confirmed_timestamp, rasta_hashing_context_t *hashing_context, const char *psk, struct key_exchange_state *kex_state, struct logger_t *logger) {
    struct RastaPacket p = initializePacket(RASTA_TYPE_KEX_REQUEST, receiver_id, sender_id, sequence_number,
                                            confirmed_sequence_number, timestamp, confirmed_timestamp, sizeof(kex_state->client_public), hashing_context);
    int ret = key_exchange_prepare_credential_request(kex_state, psk, logger);
    if (ret) {
        logger_log(logger, LOG_LEVEL_ERROR, "createKexRequest", "kex_exchange_prepare_credential_request failed!");
        abort();
    }

    memcpy(&p.data.bytes[0], kex_state->client_public, sizeof(kex_state->client_public));

    return p;
}
#else
struct RastaPacket createKexExchangeRequest(uint32_t receiver_id, uint32_t sender_id, uint32_t sequence_number, uint32_t confirmed_sequence_number,
                                            uint32_t timestamp, uint32_t confirmed_timestamp, rasta_hashing_context_t *hashing_context, const char *psk, struct key_exchange_state kex_state, struct logger_t *logger) {
    (void)receiver_id;
    (void)sender_id;
    (void)sequence_number;
    (void)confirmed_sequence_number;
    (void)timestamp;
    (void)confirmed_timestamp;
    (void)hashing_context;
    (void)psk;
    (void)kex_state;
    (void)logger;

    logger_log(logger, LOG_LEVEL_ERROR, "createKexExchangeRequest", "createKexExchangeRequest not implemented!");
    // should never be called
    abort();
}
#endif

#ifdef ENABLE_OPAQUE
struct RastaPacket createKexResponse(uint32_t receiver_id, uint32_t sender_id, uint32_t sequence_number, uint32_t confirmed_sequence_number,
                                     uint32_t timestamp, uint32_t confirmed_timestamp, rasta_hashing_context_t *hashing_context, const char *psk, const uint8_t *received_client_kex_request, size_t client_kex_request_length, uint32_t initial_sequence_number, struct key_exchange_state *kex_state, const struct RastaConfigKex *kex_config, struct logger_t *logger) {
    struct RastaPacket p = initializePacket(RASTA_TYPE_KEX_RESPONSE, receiver_id, sender_id, sequence_number,
                                            confirmed_sequence_number, timestamp, confirmed_timestamp, sizeof(kex_state->certificate_response), hashing_context);

    int ret;

    if (kex_config->has_psk_record) {
        rmemcpy(kex_state->user_record, kex_config->psk_record, sizeof(kex_state->user_record));
    } else {
        ret = key_exchange_prepare_from_psk(kex_state, psk, sender_id, receiver_id, logger);

        if (ret) {
            logger_log(logger, LOG_LEVEL_ERROR, "createKexRequest", "key_exchange_prepare_from_psk failed!");
            abort();
        }
    }

    if (client_kex_request_length != sizeof(kex_state->client_public)) {
        logger_log(logger, LOG_LEVEL_ERROR, "createKexRequest",
                   "Client sent Key Exchange Request of invalid length: %lu", client_kex_request_length);
        abort();
    }

    ret = kex_prepare_credential_response(kex_state, received_client_kex_request, client_kex_request_length, sender_id, receiver_id, initial_sequence_number, logger);

    if (ret) {
        logger_log(logger, LOG_LEVEL_ERROR, "createKexRequest", "kex_prepare_credential_response failed!");
        abort();
    }

    memcpy(&p.data.bytes[0], kex_state->certificate_response, sizeof(kex_state->certificate_response));

    return p;
}
#else
struct RastaPacket createKexExchangeResponse(uint32_t receiver_id, uint32_t sender_id, uint32_t sequence_number, uint32_t confirmed_sequence_number,
                                             uint32_t timestamp, uint32_t confirmed_timestamp, rasta_hashing_context_t *hashing_context, const char *psk, const uint8_t *received_client_kex_request, const size_t client_kex_request_length, const uint32_t initial_sequence_number, struct key_exchange_state *kex_state, const struct RastaConfigKex *kex_config, struct logger_t *logger) {
    (void)receiver_id;
    (void)sender_id;
    (void)sequence_number;
    (void)confirmed_sequence_number;
    (void)timestamp;
    (void)confirmed_timestamp;
    (void)hashing_context;
    (void)psk;
    (void)received_client_kex_request;
    (void)client_kex_request_length;
    (void)initial_sequence_number;
    (void)kex_state;
    (void)kex_config;
    (void)logger;

    logger_log(logger, LOG_LEVEL_ERROR, "createKexExchangeResponse", "createKexExchangeResponse not implemented!");
    // should never be called
    abort();
}
#endif

#ifdef ENABLE_OPAQUE
struct RastaPacket createKexAuthentication(uint32_t receiver_id, uint32_t sender_id, uint32_t sequence_number, uint32_t confirmed_sequence_number,
                                           uint32_t timestamp, uint32_t confirmed_timestamp, rasta_hashing_context_t *hashing_context, const uint8_t *user_authentication, const size_t user_authentication_length, struct logger_t *logger) {
    struct RastaPacket p = initializePacket(RASTA_TYPE_KEX_AUTHENTICATION, receiver_id, sender_id, sequence_number,
                                            confirmed_sequence_number, timestamp, confirmed_timestamp, user_authentication_length, hashing_context);
    (void)logger;
    memcpy(&p.data.bytes[0], user_authentication, user_authentication_length);

    return p;
}
#else
struct RastaPacket createKexAuthentication(uint32_t receiver_id, uint32_t sender_id, uint32_t sequence_number, uint32_t confirmed_sequence_number,
                                           uint32_t timestamp, uint32_t confirmed_timestamp, rasta_hashing_context_t *hashing_context, const uint8_t *user_authentication, const size_t user_authentication_length, struct logger_t *logger) {
    (void)receiver_id;
    (void)sender_id;
    (void)sequence_number;
    (void)confirmed_sequence_number;
    (void)timestamp;
    (void)confirmed_timestamp;
    (void)hashing_context;
    (void)user_authentication;
    (void)user_authentication_length;
    (void)logger;

    logger_log(logger, LOG_LEVEL_ERROR, "createKexAuthentication", "createKexAuthentication not implemented!");
    // should never be called
    abort();
}
#endif

struct RastaConnectionData extractRastaConnectionData(struct RastaPacket *p) {
    struct RastaConnectionData result;

    if (p->data.length == 14) {

        // extract version
        result.version[0] = p->data.bytes[0];
        result.version[1] = p->data.bytes[1];
        result.version[2] = p->data.bytes[2];
        result.version[3] = p->data.bytes[3];

        result.send_max = leShortToHost(&p->data.bytes[4]);
    } else {
        result.send_max = 0;
        for (int i = 0; i < 4; i++)
            result.version[i] = 0;
        rastafactoryLastError = RASTA_ERRORS_WRONG_PACKAGE_FORMAT;
    }

    // set null terminator correctly
    result.version[4] = '\0';

    return result;
}

struct RastaPacket createRetransmissionRequest(uint32_t receiver_id, uint32_t sender_id, uint32_t sequence_number, uint32_t confirmed_sequence_number,
                                               uint32_t timestamp, uint32_t confirmed_timestamp, rasta_hashing_context_t *hashing_context) {
    struct RastaPacket p = initializePacket(RASTA_TYPE_RETRREQ, receiver_id, sender_id, sequence_number,
                                            confirmed_sequence_number, timestamp, confirmed_timestamp, 0, hashing_context);
    return p;
}

struct RastaPacket createRetransmissionResponse(uint32_t receiver_id, uint32_t sender_id, uint32_t sequence_number, uint32_t confirmed_sequence_number,
                                                uint32_t timestamp, uint32_t confirmed_timestamp, rasta_hashing_context_t *hashing_context) {
    struct RastaPacket p = initializePacket(RASTA_TYPE_RETRRESP, receiver_id, sender_id, sequence_number,
                                            confirmed_sequence_number, timestamp, confirmed_timestamp, 0, hashing_context);
    return p;
}

struct RastaPacket createDisconnectionRequest(uint32_t receiver_id, uint32_t sender_id, uint32_t sequence_number, uint32_t confirmed_sequence_number,
                                              uint32_t timestamp, uint32_t confirmed_timestamp, struct RastaDisconnectionData data, rasta_hashing_context_t *hashing_context) {
    struct RastaPacket p = initializePacket(RASTA_TYPE_DISCREQ, receiver_id, sender_id, sequence_number,
                                            confirmed_sequence_number, timestamp, confirmed_timestamp, 4, hashing_context);

    // Details
    hostShortTole(data.details, &p.data.bytes[0]);

    // Reason
    hostShortTole(data.reason, &p.data.bytes[2]);

    return p;
}

struct RastaDisconnectionData extractRastaDisconnectionData(struct RastaPacket *p) {
    struct RastaDisconnectionData result;

    if (p->data.length == 4) {

        // details
        result.details = leShortToHost(&p->data.bytes[0]);

        // reason
        result.reason = leShortToHost(&p->data.bytes[2]);
    } else {
        result.details = 0;
        result.reason = 0;
        rastafactoryLastError = RASTA_ERRORS_WRONG_PACKAGE_FORMAT;
    }

    return result;
}

struct RastaPacket createHeartbeat(uint32_t receiver_id, uint32_t sender_id, uint32_t sequence_number, uint32_t confirmed_sequence_number,
                                   uint32_t timestamp, uint32_t confirmed_timestamp, rasta_hashing_context_t *hashing_context) {
    struct RastaPacket p = initializePacket(RASTA_TYPE_HB, receiver_id, sender_id, sequence_number,
                                            confirmed_sequence_number, timestamp, confirmed_timestamp, 0, hashing_context);
    return p;
}

struct RastaPacket createDataMessage(uint32_t receiver_id, uint32_t sender_id, uint32_t sequence_number, uint32_t confirmed_sequence_number,
                                     uint32_t timestamp, uint32_t confirmed_timestamp, struct RastaMessageData data, rasta_hashing_context_t *hashing_context) {

    unsigned int message_length = 0;

    for (unsigned int i = 0; i < data.count; i++) {
        message_length += data.data_array[i].length;
    }

    message_length = message_length + 2 * data.count;

    struct RastaPacket p = initializePacket(RASTA_TYPE_DATA, receiver_id, sender_id, sequence_number,
                                            confirmed_sequence_number, timestamp, confirmed_timestamp, message_length, hashing_context);
    // fill data array

    message_length = 0;

    for (unsigned int i = 0; i < data.count; i++) {

        hostShortTole((unsigned short)data.data_array[i].length, &p.data.bytes[0 + message_length]);

        for (unsigned int j = 0; j < data.data_array[i].length; j++) {
            p.data.bytes[2 + message_length + j] = data.data_array[i].bytes[j];
        }

        message_length += data.data_array[i].length + 2;
    }

    return p;
}

struct RastaMessageData extractMessageData(struct RastaPacket *p) {
    unsigned int current_length = 0;
    unsigned int counter = 0;

    while (current_length < p->data.length) {
        const uint16_t length = leShortToHost(&p->data.bytes[0 + current_length]);

        current_length += length + 2;
        counter++;
    }

    struct RastaMessageData result;
    allocateRastaMessageData(&result, counter);

    current_length = 0;

    for (unsigned int i = 0; i < result.count; i++) {
        const uint16_t length = leShortToHost(&p->data.bytes[0 + current_length]);

        allocateRastaByteArray(&result.data_array[i], length);

        for (int j = 0; j < length; j++) {
            result.data_array[i].bytes[j] = p->data.bytes[2 + current_length + j];
        }

        current_length += length + 2;
    }

    return result;
}

struct RastaPacket createRetransmittedDataMessage(uint32_t receiver_id, uint32_t sender_id, uint32_t sequence_number, uint32_t confirmed_sequence_number,
                                                  uint32_t timestamp, uint32_t confirmed_timestamp, struct RastaMessageData data, rasta_hashing_context_t *hashing_context) {
    struct RastaPacket result;
    result = createDataMessage(receiver_id, sender_id, sequence_number, confirmed_sequence_number,
                               timestamp, confirmed_timestamp, data, hashing_context);

    result.type = RASTA_TYPE_RETRDATA;

    return result;
}

void createRedundancyPacket(uint32_t sequence_number, struct RastaPacket *inner_data, struct crc_options checksum_type, struct RastaRedundancyPacket *packet) {
    packet->sequence_number = sequence_number;
    packet->data = *inner_data;
    packet->checksum_type = checksum_type;

    // reserved bytes have to be 0s in version 03.03
    packet->reserve = 0x0000;

    // length = 2 bytes length field + 2 bytes reserve + 4 bytes seq. nr. + inner data length + checksum length
    // checksum width in crc_options is in bit, so divide by 8 for bytes
    packet->length = (uint16_t)(8 + inner_data->length + (checksum_type.width / 8));

    // set checksum_correct to 1 as checksum will be calculated on conversion to bytes
    packet->checksum_correct = 1;
}
