#include <netinet/in.h>
#include <stdint.h>

#include <rasta/config.h>

#include "transport.h"

typedef struct rasta_transport_socket rasta_transport_socket;
typedef struct rasta_transport_channel rasta_transport_channel;

/**
 * Prepare to accept connections on the given @p file_descriptor.
 * @param file_descriptor the file descriptor to accept connections from
 */
void tcp_listen(rasta_transport_socket *transport_socket);

/**
 * Receive data on the given @p file descriptor and store it in the given buffer.
 * This function will block until data is received!
 * @param ssl the wolfssl session object
 * @param received_message a buffer where the received data will be written too. Has to be at least \p max_buffer_len long
 * @param max_buffer_len the amount of data which will be received in bytes
 * @param sender information about the sender of the data will be stored here
 * @return the amount of received bytes
 */
ssize_t tcp_receive(rasta_transport_channel *transport_channel, unsigned char *received_message, size_t max_buffer_len, struct sockaddr_in *sender);

/**
 * Await a connection on a @p file_descriptor.
 * When a connection arrives, open a new socket to communicate with it,
 * @param file_descriptor the file descriptor to accept connections from
 */
int tcp_accept(rasta_transport_socket *transport_socket);

/**
 * Open a connection on a @p file_descriptor.
 * When a connection arrives, open a new socket to communicate with it,
 * @param file_descriptor the file descriptor to accept connections from
 * @param host the host where the message will be send to. This has to be an IPv4 address in the format a.b.c.d
 * @param port the target port on the host
 */
int tcp_connect(rasta_transport_channel *channel);

/**
 * Sends a message via the given file descriptor to a @p host and @p port
 * @param file_descriptor the file descriptor which is used to send the message
 * @param message the message that which will be send
 * @param message_len the length of the @p message
 */
void tcp_send(rasta_transport_channel *transport_channel, unsigned char *message, size_t message_len);

/**
 * Closes the tcp socket
 * @param transport_state the transport state either the filedescriptor (tcp) or the ssl session (tls)
 */
void tcp_close(rasta_transport_channel *transport_channel);

/**
 * Closes the listening/connecting socket and releases its resources
 * @param transport_socket the socket to close
 */
void tcp_close_socket(rasta_transport_socket *transport_socket);

void get_client_addr_from_socket(const rasta_transport_socket *transport_socket, struct sockaddr_in *client_addr, socklen_t *addr_len);
