#pragma once

#include <rasta/config.h>
#include <stdint.h>

/**
 * the RaSTA version that is implemented
 */
#define RASTA_VERSION "0303"

#define NS_PER_SEC 1000000000ULL
#define MS_PER_S 1000ULL
#define NS_PER_MS 1000000ULL

uint64_t get_current_time_ms();

int compare_version(char (*local_version)[5], char (*remote_version)[5]);
int version_accepted(rasta_config_info *config, char (*version)[5]);
