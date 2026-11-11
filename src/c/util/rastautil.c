#include "rastautil.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rmemory.h"

#define rasta_htole32(X) (X)
#define rasta_le32toh(X) (X)

/**
 * this will generate a 4 byte timestamp of the current system time
 * @return current system time in milliseconds since the boot time (on Linux, behaviour on other systems may differ)
 */
uint32_t cur_timestamp() {
    long ms;
    time_t s;
    struct timespec spec;

    clock_gettime(CLOCK_MONOTONIC, &spec);

    s = spec.tv_sec;

    // seconds to milliseconds
    ms = s * MS_PER_S;

    // nanoseconds to milliseconds
    ms += (long)(spec.tv_nsec / 1.0e6);

    return (uint32_t)ms;
}

void freeRastaByteArray(struct RastaByteArray *data) {
    data->length = 0;
    rfree(data->bytes);
}

void allocateRastaByteArray(struct RastaByteArray *data, unsigned int length) {
    data->bytes = rmalloc(length);
    memset(data->bytes, 0, length);
    data->length = length;
}

int isBigEndian() {
    /*unsigned short t = 0x0102;
    return (t & 0xFF) == 0x02 ? 1 : 0;*/
    int i = 1;
    return !*((char *)&i);
}

void hostLongToLe(uint32_t v, unsigned char *result) {
    v = rasta_htole32(v);
    rmemcpy(result, &v, sizeof(v));
}

uint32_t leLongToHost(const unsigned char v[4]) {
    uint32_t result;
    rmemcpy(&result, v, sizeof(result));
    return rasta_le32toh(result);
}
