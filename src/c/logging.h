#pragma once
/**
 * This module provides basic logging functionality
 * logging to a file, to the console or both are supported
 *
 * The supported log level are
 * Debug, Info, Error (and None which can only be used to specify the maximum log level)
 */

#include <stdio.h>
#include <rasta/config.h>
#include "util/fifo.h"

#define LOG_FORMAT "[%s][%s][%s] %s\n"

/**
 * maximum amount of log messages in the buffer
 */
#define LOGGER_BUFFER_SIZE 10

/**
 * maximum size of log messages in bytes
 */
#define LOGGER_MAX_MSG_SIZE 4096

/**
 * wrapper struct to pass multiple parameters to the write thread handler
 */
typedef struct {
    /**
     * the buffer FIFO
     */
    fifo_t *buffer;

    /**
     * if logger is Console or Both: path to the log file. NULL otherwise
     */
    char *log_file;

    /**
     * the type of the logger
     */
    logger_type type;
} write_thread_parameter_wrapper;

/**
 * represents a logger
 */
struct logger_t {
    /**
     * maximum log level the logger will log
     */
    log_level max_log_level;

    /**
     * the type of logging that will be used
     */
    logger_type type;

    /**
     * the path to the log file, when file logging is used
     */
    char *log_file;
};

/**
 * initializes the logger
 * @param max_log_level the maximum log level the will be logged, oder is DEBUG>INFO>ERROR>NONE
 * @param type the type of logging
 * @return a logger struct
 */
void logger_init(struct logger_t *logger, log_level max_log_level, logger_type type);

/**
 * sets the path to the log file
 * @param logger the logger where the file should be set
 * @param path the path to the file
 */
void logger_set_log_file(struct logger_t *logger, char *path);

/**
 * logs a message
 * @param logger the logger which should be used
 * @param level the log level of the message
 * @param location the location where the log message occurred
 * @param format the message which should be logged. can contain formatting information like %s, %d, ...
 * @param ... the format parameters
 */
void logger_log(struct logger_t *logger, log_level level, char *location, char *format, ...) __attribute__((format(printf, 4, 5)));

/**
 * logs a message of a specified condition is true (1)
 * @param logger the logger which should be used
 * @param cond the message will only be logged if this expression is 1
 * @param level the log level of the message
 * @param location the location where the log message occurred
 * @param format the message which should be logged. can contain formatting information like %s, %d, ...
 * @param ... the format parameters
 */
void logger_log_if(struct logger_t *logger, int cond, log_level level, char *location, char *format, ...) __attribute__((format(printf, 5, 6)));

/**
 * Print a description, followed by a memory range in hex and ascii
 * @param logger the logger which should be used
 * @param level the log level of the message
 * @param data pointer to data_length consecutive bytes
 * @param data_length number of bytes to be printed
 * @param header_fmt format for an extra header, can contain formatting information like %s, %d, ...
 * @param ... format parameters
 */
void logger_hexdump(struct logger_t *logger, log_level level, const void *data, size_t data_length, char *header_fmt, ...);
