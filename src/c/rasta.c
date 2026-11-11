#include <rasta/rasta.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <rasta/rasta.h>

#include "experimental/handlers.h"
#include "rasta_connection.h"
#include "rastahandle.h"
#include "retransmission/handlers.h"
#include "retransmission/safety_retransmission.h"
#include "util/event_system.h"
#include "util/rmemory.h"

void log_main_loop_state(struct rasta_handle *h, event_system *ev_sys, const char *message) {
    int fd_event_count = 0, fd_event_active_count = 0, timed_event_count = 0, timed_event_active_count = 0;
    for (fd_event *ev = ev_sys->fd_events.first; ev; ev = ev->next) {
        fd_event_count++;
        fd_event_active_count += !!ev->enabled;
    }
    for (timed_event *ev = ev_sys->timed_events.first; ev; ev = ev->next) {
        timed_event_count++;
        timed_event_active_count += !!ev->enabled;
    }
    logger_log(h->logger, LOG_LEVEL_DEBUG, "RaSTA EVENT-SYSTEM", "%s | %d/%d fd events and %d/%d timed events active",
               message, fd_event_active_count, fd_event_count, timed_event_active_count, timed_event_count);
}

bool rasta_bind(rasta *user_configuration) {
    return redundancy_mux_bind(&user_configuration->h);
}

void rasta_listen(rasta *user_configuration) {
    sr_listen(&user_configuration->h);
}

rasta_connection *rasta_accept(rasta *user_configuration) {
    struct rasta_handle *h = &user_configuration->h;
    event_system *event_system = &user_configuration->rasta_lib_event_system;

    // Re-initialize the mux
    redundancy_mux_init(&h->mux);

    // accept events were already prepared by rasta_listen
    // event system will break when we have received the first heartbeat of a new connection
    log_main_loop_state(h, event_system, "event-system started");
    event_system_start(event_system);

    if (h->rasta_connection->is_new) {
        h->rasta_connection->is_new = false;
        return h->rasta_connection;
    }

    return NULL;
}

int terminator_callback(void *carry, int fd) {
    rasta *r = carry;

    logger_log(&r->logger, LOG_LEVEL_DEBUG, "RaSTA Cancel", "Executing cancel handler...");

    // Invalidate the event (read from the pipe)
    uint64_t u;
    ssize_t ignored = read(fd, &u, sizeof(u));
    (void)ignored;

    // Close the pipe
    close(fd);

    // Exit the event loop
    return 1;
}

typedef struct rasta_cancellation {
    int fd[2];
} rasta_cancellation;

rasta_cancellation *rasta_prepare_cancellation(rasta *r) {
    logger_log(&r->logger, LOG_LEVEL_DEBUG, "RaSTA Cancel", "Allocating cancellation...");
    rasta_cancellation *result = rmalloc(sizeof(rasta_cancellation));

    // Cancel event
    if (pipe(result->fd) < 0) {
        perror("Failed to create pipe");
        rfree(result);
        return NULL;
    }

    return result;
}

rasta_connection *rasta_accept_with_cancel(rasta *r, rasta_cancellation *cancellation) {
    logger_log(&r->logger, LOG_LEVEL_DEBUG, "RaSTA Accept", "Registering cancel event...");

    fd_event terminator_event;
    memset(&terminator_event, 0, sizeof(fd_event));
    terminator_event.callback = terminator_callback;
    terminator_event.carry_data = r;
    terminator_event.fd = cancellation->fd[0];
    enable_fd_event(&terminator_event);
    rasta_add_fd_event(r, &terminator_event, EV_READABLE);

    rasta_connection *result = rasta_accept(r);

    logger_log(&r->logger, LOG_LEVEL_DEBUG, "RaSTA Accept", "Unregistering cancel event...");

    rasta_remove_fd_event(r, &terminator_event);
    close(cancellation->fd[1]);

    logger_log(&r->logger, LOG_LEVEL_DEBUG, "RaSTA Cancel", "Freeing cancellation...");
    rfree(cancellation);

    return result;
}

void rasta_cancel_operation(rasta *r, rasta_cancellation *cancel) {
    logger_log(&r->logger, LOG_LEVEL_DEBUG, "RaSTA Cancel", "Canceling operation...");

    uint64_t terminate = 1;
    uint64_t ignore = write(cancel->fd[1], &terminate, sizeof(uint64_t));
    (void)ignore;
}

rasta_connection *rasta_connect(rasta *user_configuration) {
    return sr_connect(&user_configuration->h);
}

int rasta_recv(rasta *user_configuration, rasta_connection *connection, void *buf, size_t len) {
    struct rasta_handle *h = &user_configuration->h;
    event_system *event_system = &user_configuration->rasta_lib_event_system;

    while (connection->current_state == RASTA_CONNECTION_UP && sr_recv_queue_item_count(connection) == 0) {
        log_main_loop_state(h, event_system, "event-system started");
        event_system_start(event_system);
    }

    if (connection->current_state != RASTA_CONNECTION_UP) {
        // TODO: If sockets are broken, their event handlers have to be removed...
        return -1;
    }

    struct RastaByteArray *elem;
    elem = fifo_pop(connection->fifo_receive);
    size_t received_len = (len < elem->length) ? len : elem->length;

    if (len < elem->length) {
        logger_log(connection->logger, LOG_LEVEL_INFO, "RaSTA receive",
                   "supplied buffer (%zd bytes) is smaller than message length (%d bytes) - received message may be incomplete!", len, elem->length);
    }

    rmemcpy(buf, elem->bytes, received_len);
    freeRastaByteArray(elem);
    rfree(elem);

    return received_len;
}

int rasta_send(rasta *user_configuration, rasta_connection *connection, void *buf, size_t len) {
    struct RastaMessageData messageData1;
    allocateRastaMessageData(&messageData1, 1);
    messageData1.data_array[0].bytes = buf;
    messageData1.data_array[0].length = len;

    int return_val = sr_send(&user_configuration->h, connection, messageData1);
    rfree(messageData1.data_array);
    return return_val;
}

void rasta_disconnect(rasta_connection *connection) {
    sr_disconnect(connection);
}

static void destroy_byte_array_fifo(fifo_t **fifo) {
    struct RastaByteArray *elem;
    while ((elem = fifo_pop(*fifo))) {
        freeRastaByteArray(elem);
        rfree(elem);
    }
    fifo_destroy(fifo);
}

void rasta_cleanup(rasta *user_configuration) {
    sr_cleanup(&user_configuration->h);

    // All three queues hold allocated byte arrays
    destroy_byte_array_fifo(&user_configuration->h.rasta_connection->fifo_retransmission);
    destroy_byte_array_fifo(&user_configuration->h.rasta_connection->fifo_send);
    destroy_byte_array_fifo(&user_configuration->h.rasta_connection->fifo_receive);

    rfree(user_configuration->h.rasta_connection);
    rfree(user_configuration);
}
