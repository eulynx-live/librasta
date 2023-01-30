#include "../headers/safety_retransmission_test.h"
#include <CUnit/Basic.h>
#include <rasta/rastahandle.h>
#include <rasta/rastaredundancy.h>
#include <rasta/rastautil.h>
#include <rasta/rmemory.h>

#define SERVER_ID 0xA

void sr_retransmit_data(struct rasta_receive_handle *h, struct rasta_connection *connection);

static fifo_t* test_send_fifo = NULL;

void fake_send_callback(redundancy_mux *mux, struct RastaByteArray data_to_send, rasta_transport_channel *channel, unsigned int channel_index) {
    if (test_send_fifo == NULL) {
        test_send_fifo = fifo_init(128);
    }

    struct RastaByteArray *to_fifo = rmalloc(sizeof(struct RastaByteArray));
    allocateRastaByteArray(to_fifo, data_to_send.length);
    rmemcpy(to_fifo->bytes, data_to_send.bytes, data_to_send.length);

    fifo_push(test_send_fifo, to_fifo);
}

void test_sr_retransmit_data_shouldSendFinalHeartbeat() {
    fifo_destroy(test_send_fifo);

    struct rasta_receive_handle h;
    struct logger_t logger = logger_init(LOG_LEVEL_INFO, LOGGER_TYPE_CONSOLE);
    h.logger = &logger;

    struct RastaConfigInfo info;
    struct RastaConfigInfoRedundancy configInfoRedundancy;
    configInfoRedundancy.t_seq = 100;
    configInfoRedundancy.n_diagnose = 10;
    configInfoRedundancy.crc_type = crc_init_opt_a();
    configInfoRedundancy.n_deferqueue_size = 2;
    info.redundancy = configInfoRedundancy;

    uint16_t listenPorts[1] = {1234};
    redundancy_mux mux = redundancy_mux_init(logger, listenPorts, 1, info);
    redundancy_mux_set_config_id(&mux, SERVER_ID);
    h.mux = &mux;

    rasta_redundancy_channel fake_channel;
    fake_channel.associated_id = SERVER_ID;
    fake_channel.hashing_context.hash_length = RASTA_CHECKSUM_NONE;
    rasta_md4_set_key(&fake_channel.hashing_context, 0, 0, 0, 0);

    rasta_transport_channel transport;
    transport.send_callback = fake_send_callback;
    fake_channel.connected_channels = &transport;
    fake_channel.connected_channel_count = 1;

    mux.connected_channels = &fake_channel;
    mux.channel_count = 1;

    struct rasta_connection connection;
    connection.remote_id = SERVER_ID;
    connection.fifo_retransmission = fifo_init(0);

    sr_retransmit_data(&h, &connection);

    // One message should be sent
    CU_ASSERT_EQUAL(1, fifo_get_size(test_send_fifo));

    struct RastaByteArray* hb_message = fifo_pop(test_send_fifo);
    CU_ASSERT_EQUAL(36, hb_message->length);
    // 8 bytes retransmission header, 2 bytes offset for message type
    CU_ASSERT_EQUAL(RASTA_TYPE_HB, leShortToHost(hb_message->bytes + 8 + 2));

    fifo_destroy(connection.fifo_retransmission);
    logger_destroy(&logger);
}

void test_sr_retransmit_data_shouldRetransmitPackage() {
    fifo_destroy(test_send_fifo);

    // Arrange
    struct rasta_receive_handle h;
    struct logger_t logger = logger_init(LOG_LEVEL_INFO, LOGGER_TYPE_CONSOLE);
    h.logger = &logger;

    struct RastaConfigInfo info;
    struct RastaConfigInfoRedundancy configInfoRedundancy;
    configInfoRedundancy.t_seq = 100;
    configInfoRedundancy.n_diagnose = 10;
    configInfoRedundancy.crc_type = crc_init_opt_a();
    configInfoRedundancy.n_deferqueue_size = 2;
    info.redundancy = configInfoRedundancy;

    uint16_t listenPorts[1] = {1234};
    redundancy_mux mux = redundancy_mux_init(logger, listenPorts, 1, info);
    redundancy_mux_set_config_id(&mux, SERVER_ID);
    h.mux = &mux;

    rasta_redundancy_channel fake_channel;
    fake_channel.associated_id = SERVER_ID;
    fake_channel.hashing_context.hash_length = RASTA_CHECKSUM_NONE;
    rasta_md4_set_key(&fake_channel.hashing_context, 0, 0, 0, 0);

    rasta_transport_channel transport;
    transport.send_callback = fake_send_callback;
    fake_channel.connected_channels = &transport;
    fake_channel.connected_channel_count = 1;

    mux.connected_channels = &fake_channel;
    mux.channel_count = 1;

    struct rasta_connection connection;
    connection.remote_id = SERVER_ID;
    connection.fifo_retransmission = fifo_init(1);

    struct RastaMessageData app_messages;
    struct RastaByteArray message;
    message.bytes = "Hello world";
    message.length = strlen(message.bytes) + 1;
    app_messages.count = 1;
    app_messages.data_array = &message;

    rasta_hashing_context_t hashing_context;
    hashing_context.hash_length = RASTA_CHECKSUM_NONE;
    rasta_md4_set_key(&hashing_context, 0, 0, 0, 0);
    h.hashing_context = &hashing_context;

    struct RastaPacket data = createDataMessage(SERVER_ID, 0, 0, 0, 0, 0, app_messages, &hashing_context);
    struct RastaByteArray packet = rastaModuleToBytes(data, &hashing_context);
    struct RastaByteArray *to_fifo = rmalloc(sizeof(struct RastaByteArray));
    allocateRastaByteArray(to_fifo, packet.length);
    rmemcpy(to_fifo->bytes, packet.bytes, packet.length);
    fifo_push(connection.fifo_retransmission, to_fifo);

    // Act
    sr_retransmit_data(&h, &connection);

    // Assert

    // Retranmission queue should still contain 1 (unconfirmed) packet
    CU_ASSERT_EQUAL(1, fifo_get_size(connection.fifo_retransmission));

    // Two messages should be sent
    CU_ASSERT_EQUAL(2, fifo_get_size(test_send_fifo));

    struct RastaByteArray* retrdata_message = fifo_pop(test_send_fifo);
    CU_ASSERT_EQUAL(8 + 42, retrdata_message->length);
    CU_ASSERT_EQUAL(RASTA_TYPE_RETRDATA, leShortToHost(retrdata_message->bytes + 8 + 2));
    // Contains 'Hello world'
    CU_ASSERT_EQUAL(message.length, leShortToHost(retrdata_message->bytes + 8 + 28));
    CU_ASSERT_EQUAL(0, memcmp(retrdata_message->bytes + 8 + 28 + 2, message.bytes, message.length));

    struct RastaByteArray* hb_message = fifo_pop(test_send_fifo);
    CU_ASSERT_EQUAL(8 + 28, hb_message->length);
    CU_ASSERT_EQUAL(RASTA_TYPE_HB, leShortToHost(hb_message->bytes + 8 + 2));

    fifo_destroy(connection.fifo_retransmission);
    logger_destroy(&logger);
}