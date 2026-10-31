#pragma once

#include <stdint.h>

#define NS_PER_S 1000000000ULL
#define MS_PER_S 1000ULL
#define NS_PER_MS 1000000ULL

struct RastaByteArray {
    unsigned char *bytes;
    unsigned int length;
};

/**
 * Frees the bytes array, sets the pointer to NULL and length to 0. Freeing the same array again does nothing.
 * @param data the byte array
 */
void freeRastaByteArray(struct RastaByteArray *data);

/**
 * Allocates space for the bytearray
 * @param data the data
 * @param length the length
 */
void allocateRastaByteArray(struct RastaByteArray *data, unsigned int length);

/**
 * this will generate a 4 byte timestamp of the current system time
 * @return current system time in s since 1970
 */
uint32_t cur_timestamp();

int isBigEndian();

/**
 * Converts a unsigned long into a uchar array
 * @param v the uchar array
 * @param result the assigned uchar array; length should be 4
 */
void hostLongToLe(uint32_t v, unsigned char *result);

/**
 * Converts 4 little-endian bytes to a host ulong
 * @param v pointer to uint32_t in LE byte order
 * @return the ulong
 */
uint32_t leLongToHost(const unsigned char v[4]);
