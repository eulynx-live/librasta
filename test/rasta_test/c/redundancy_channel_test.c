
#include "CUnit/Basic.h"

#include <rasta/rasta.h>

#include "../../../src/c/rastahandle.h"
#include "../../../src/c/redundancy/rasta_redundancy_channel.h"

void test_redundancy_channel() {
    struct rasta_handle rasta_h = {0};

    rasta_config_info info = {0};
    info.redundancy.t_seq = 100;
    info.redundancy.n_diagnose = 10;
    info.redundancy.crc_type = crc_init_opt_a();
    info.redundancy.n_deferqueue_size = 2;

    struct logger_t logger = {0};
    logger_init(&logger, LOG_LEVEL_INFO, LOGGER_TYPE_CONSOLE);

    redundancy_mux mux = {0};
    redundancy_mux_alloc(&rasta_h, &mux, &logger, &info);

    rasta_redundancy_channel channel;
    redundancy_channel_alloc(&rasta_h, &logger, &info, &channel);
    int result = redundancy_channel_connect(&mux, &channel);

    CU_ASSERT_EQUAL(result, 1);

    redundancy_channel_free(&channel);
    redundancy_mux_close(&mux);
}
