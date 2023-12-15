#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rasta/rasta.h>

#include "configfile.h"

#define CONFIG_PATH_S "rasta_server_local.cfg"
#define CONFIG_PATH_C "rasta_client_local.cfg"

#define ID_R 0x61
#define ID_S 0x60

#define BUF_SIZE 500

void printHelpAndExit(void) {
    printf("Invalid Arguments!\n use 'r' to start in receiver mode and 's' to start in sender mode.\n");
    exit(1);
}

struct connect_event_data {
    rasta *rc;
    struct rasta_connection *connection;
};

int send_input_data(void *carry_data) {
    struct connect_event_data *data = carry_data;
    char buf[BUF_SIZE];
    int c;

    for (;;) {
        size_t read_len = 0;
        while (read_len < BUF_SIZE) {
            c = getchar();

            if (c == EOF) {
                if (read_len > 0) {
                    rasta_send(data->rc, data->connection, buf, read_len);
                }
                rasta_disconnect(data->connection);
                return 1;
            }

            buf[read_len++] = c;

            if (c == '\n') {
                rasta_send(data->rc, data->connection, buf, read_len);
                return 0;
            }
        }
        rasta_send(data->rc, data->connection, buf, read_len);
    }
}

int main(int argc, char *argv[]) {
    bool force_disable_rekeying = false;
    if (argc < 2) printHelpAndExit();

    if (argc > 2) {
        force_disable_rekeying = true;
        printf("Disabling rekeying!");
    }

    rasta* rc = NULL;

    rasta_ip_data toServer[2];

    fd_event input_available_event;
    struct connect_event_data input_available_event_data;

    memset(&input_available_event, 0, sizeof(fd_event));

    input_available_event.callback = send_input_data;
    input_available_event.carry_data = &input_available_event_data;
    input_available_event.fd = STDIN_FILENO;

    char buf[BUF_SIZE];

    if (strcmp(argv[1], "r") == 0) {
        printf("->   R (ID = 0x%lX)\n", (unsigned long)ID_R);
        rasta_config_info config;
        struct logger_t logger;
        load_configfile(&config, &logger, CONFIG_PATH_S);

        strcpy(toServer[0].ip, "127.0.0.1");
        strcpy(toServer[1].ip, "127.0.0.1");
        toServer[0].port = 9998;
        toServer[1].port = 9999;

        rasta_connection_config connection = {
            .config = &config,
            .rasta_id = ID_S,
            .transport_sockets = toServer,
            .transport_sockets_count = sizeof(toServer) / sizeof(toServer[0])};

        rasta_lib_init_configuration(rc, &config, &connection, 1, LOG_LEVEL_DEBUG, LOGGER_TYPE_CONSOLE);

        if (force_disable_rekeying) {
            rc->h.config->kex.rekeying_interval_ms = 0;
        }

        rasta_bind(rc);

        rasta_listen(rc);

        struct rasta_connection *c = rasta_accept(rc);
        if (c == NULL) {
            printf("Could not accept connection\n");
            exit(1);
        }

        // TODO: Terrible API
        input_available_event_data.rc = rc;
        input_available_event_data.connection = c;

        enable_fd_event(&input_available_event);
        rasta_add_fd_event(rc, &input_available_event, EV_READABLE);

        ssize_t recv_len;
        while ((recv_len = rasta_recv(rc, c, buf, BUF_SIZE)) > 0) {
            // write to stdout
            if (write(STDOUT_FILENO, buf, recv_len) == -1) {
                break;
            }
        }
    } else if (strcmp(argv[1], "s") == 0) {
        printf("->   S (ID = 0x%lX)\n", (unsigned long)ID_S);
        rasta_config_info config;
        struct logger_t logger;
        load_configfile(&config, &logger, CONFIG_PATH_C);

        strcpy(toServer[0].ip, "127.0.0.1");
        strcpy(toServer[1].ip, "127.0.0.1");
        toServer[0].port = 8888;
        toServer[1].port = 8889;

        rasta_connection_config connection = {
            .config = &config,
            .rasta_id = ID_R,
            .transport_sockets = toServer,
            .transport_sockets_count = sizeof(toServer) / sizeof(toServer[0])};

        rasta_lib_init_configuration(rc, &config, &connection, 1, LOG_LEVEL_DEBUG, LOGGER_TYPE_CONSOLE);

        if (force_disable_rekeying) {
            rc->h.config->kex.rekeying_interval_ms = 0;
        }

        rasta_bind(rc);

        struct rasta_connection *c = rasta_connect(rc, ID_R);

        if (c == NULL) {
            printf("->   Failed to connect any channel.\n");
            return 1;
        };

        // TODO: Terrible API
        input_available_event_data.rc = rc;
        input_available_event_data.connection = c;

        enable_fd_event(&input_available_event);
        rasta_add_fd_event(rc, &input_available_event, EV_READABLE);

        ssize_t recv_len;
        while ((recv_len = rasta_recv(rc, c, buf, BUF_SIZE)) > 0) {
            if (write(STDOUT_FILENO, buf, recv_len) == -1) {
                break;
            }
        }
    }

    rasta_cleanup(rc);
    return 0;
}
