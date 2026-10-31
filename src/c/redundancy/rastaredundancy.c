#include "rastaredundancy.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../rasta_connection.h"
#include "../rastahandle.h"
#include "../retransmission/safety_retransmission.h"
#include "../transport/transport.h"
#include "../util/rastadeferqueue.h"
#include "../util/rmemory.h"
#include "rasta_redundancy_channel.h"

int _deliver_message_to_upper_layer(rasta_connection *h, rasta_redundancy_channel *channel, struct RastaByteArray message) {
    struct RastaPacket packet;
    bytesToRastaPacket(message, &channel->hashing_context, &packet);
    return sr_receive(h, &packet);
}

/**
 * delivers a message in the defer queue to next layer i.e. adds it to the receive buffer
 * see 6.6.4.4.6 for more details
 * @param connection the connection data which is used
 */
int red_f_deliverDeferQueue(rasta_connection *con, rasta_redundancy_channel *channel) {
    int result = 0;
    logger_log(channel->logger, LOG_LEVEL_DEBUG, "RaSTA Red deliver deferq", "f_deliverDeferQueue called");

    // check if message with seq_pdu == seq_rx in defer queue
    while (deferqueue_contains(&channel->defer_q, channel->seq_rx)) {
        logger_log(channel->logger, LOG_LEVEL_DEBUG, "RaSTA Red deliver deferq", "deferq contains seq_pdu=%lu",
                   channel->seq_rx);

        struct RastaByteArray innerPackerBytes;
        // convert inner data (RaSTA SR layer PDU) to byte array
        struct RastaRedundancyPacket queuePacket = deferqueue_get(&channel->defer_q, channel->seq_rx);
        innerPackerBytes = rastaModuleToBytes(&queuePacket.data, &channel->hashing_context);
        result |= _deliver_message_to_upper_layer(con, channel, innerPackerBytes);
        freeRastaByteArray(&innerPackerBytes);

        // remove message from queue (effectively a pop operation with the get call)
        deferqueue_remove(&channel->defer_q, channel->seq_rx);
        logger_log(channel->logger, LOG_LEVEL_DEBUG, "RaSTA Red deliver deferq", "remove message from deferq");

        freeRastaByteArray(&queuePacket.data.data);
        freeRastaByteArray(&queuePacket.data.checksum);

        // increase seq_rx
        channel->seq_rx++;
    }
    logger_log(channel->logger, LOG_LEVEL_DEBUG, "RaSTA Red deliver deferq", "deferq doesn't contain seq_pdu=%lu",
               channel->seq_rx);
    return result;
}

/**
 * Checks a received packet and adds it to the defer queue if it has to be delivered.
 * Doesn't free discarded packets, see red_f_receiveData.
 * @return 1 if the packet has the next expected sequence number, 0 otherwise
 */
static int red_f_processData(rasta_redundancy_channel *channel, struct RastaRedundancyPacket packet, int channel_id) {
    int result = 0;

    logger_log(channel->logger, LOG_LEVEL_DEBUG, "RaSTA Red receive", "Channel %d: ptr=%p", channel_id, (void *)channel);

    if (!packet.checksum_correct) {
        logger_log(channel->logger, LOG_LEVEL_DEBUG, "RaSTA Red receive", "Channel 0: Packet checksum incorrect on channel %d", channel_id);

        // checksum incorrect, exit function
        return 0;
    }

    // else checksum correct

    { // Diagnostics
        // increase amount of received packets of this channel
        channel->transport_channels[channel_id].diagnostics_data.received_packets += 1;
    }

    // only accept pdu with seq. nr = 0 as first message
    if (channel->seq_rx == 0 && channel->seq_tx == 0 && packet.sequence_number != 0) {
        logger_log(channel->logger, LOG_LEVEL_DEBUG, "RaSTA Red receive", "channel %d: first seq_pdu != 0", channel_id);

        return 0;
    }

    // check seq_pdu
    if (packet.sequence_number < channel->seq_rx) {
        logger_log(channel->logger, LOG_LEVEL_DEBUG, "RaSTA Red receive", "channel %d: seq_pdu < seq_rx", channel_id);
        // message has been received by other transport channel

        { // Diagnostics
            // -> calculate delay by looking for the received ts in diagnostics queue

            unsigned long ts = deferqueue_get_ts(&channel->diagnostics_packet_buffer, packet.sequence_number);
            if (ts != 0) {
                // seq_pdu was in queue, received time is ts
                unsigned long delay = cur_timestamp() - ts;

                // if delay > T_SEQ, message is late
                if (delay > channel->configuration_parameters.t_seq) {
                    // channel is late, increase missed counter
                    channel->transport_channels[channel_id].diagnostics_data.n_missed++;
                } else {
                    // update t_drift and t_drift2
                    channel->transport_channels[channel_id].diagnostics_data.t_drift += delay;
                    channel->transport_channels[channel_id].diagnostics_data.t_drift2 += (delay * delay);
                }
            }
        }

        // discard message
        return 0;
    } else if (packet.sequence_number == channel->seq_rx) {
        logger_log(channel->logger, LOG_LEVEL_DEBUG, "RaSTA Red receive", "channel %d: correct seq. nr. delivering to next layer",
                   channel_id);
        logger_log(channel->logger, LOG_LEVEL_DEBUG, "RaSTA Red receive", "channel %d: seq_pdu=%lu, seq_rx=%lu",
                   channel_id, (long unsigned int)packet.sequence_number, channel->seq_rx - 1);

        { // Diagnostics
            // received packet as first transport channel -> add with ts to diagnostics buffer
            if (!deferqueue_add(&channel->diagnostics_packet_buffer, packet, cur_timestamp())) {
                logger_log(channel->logger, LOG_LEVEL_INFO, "RaSTA Red receive", "diagnostics packet buffer is full");
            }
        }

        // Here we deviate a bit from the flow diagram in the docs, because also insert in-order
        // packets into the defer queue. This is to support the case where we receive data from
        // connections that we don't currently rasta_recv from

        if (!deferqueue_add(&channel->defer_q, packet, cur_timestamp())) {
            logger_log(channel->logger, LOG_LEVEL_INFO, "RaSTA Red receive", "discarded packet because defer queue was full");
        }

        return 1;
    } else if (channel->seq_rx < packet.sequence_number && packet.sequence_number <= (channel->seq_rx + channel->configuration_parameters.n_deferqueue_size * 10)) {
        logger_log(channel->logger, LOG_LEVEL_DEBUG, "RaSTA Red receive", "channel %d: seq_rx < seq_pdu && seq_pdu <= (seq_rx + 10 * MAX_DEFERQUEUE_SIZE)", channel_id);

        // check if message is in defer queue
        if (deferqueue_contains(&channel->defer_q, packet.sequence_number)) {
            logger_log(channel->logger, LOG_LEVEL_DEBUG, "RaSTA Red receive", "channel %d: packet already in deferq",
                       channel_id);

            // discard message
            // possibly statistic analysis
            return 0;
        } else {
            // check if queue is full
            if (deferqueue_isfull(&channel->defer_q)) {
                logger_log(channel->logger, LOG_LEVEL_INFO, "RaSTA Red receive", "channel %d: deferq full", channel_id);

                // full -> discard message
                return 0;
            } else {
                logger_log(channel->logger, LOG_LEVEL_DEBUG, "RaSTA Red receive", "channel %d: adding message to deferq",
                           channel_id);

                // add message to defer queue
                if (!deferqueue_add(&channel->defer_q, packet, cur_timestamp())) {
                    logger_log(channel->logger, LOG_LEVEL_INFO, "RaSTA Red receive", "discarded packet because defer queue was full");
                }
            }
        }
    } else if (packet.sequence_number > (channel->seq_rx + channel->configuration_parameters.n_deferqueue_size * 10)) {
        logger_log(channel->logger, LOG_LEVEL_DEBUG, "RaSTA Red receive", "channel %d: seq_pdu > seq_rx + 10 * MAX_DEFERQUEUE_SIZE", channel_id);

        // discard message
        return 0;
    }

    return result;
}

int red_f_receiveData(rasta_redundancy_channel *channel, struct RastaRedundancyPacket packet, int channel_id) {
    unsigned int deferred_count = channel->defer_q.count;
    int result = red_f_processData(channel, packet, channel_id);

    // The defer queue takes ownership of the packet's byte arrays and frees them on delivery.
    // Discarded packets (e.g. duplicates received on another transport channel) have to be freed here.
    // length is 0 if bytesToRastaPacket rejected the packet before allocating anything.
    if (channel->defer_q.count == deferred_count && packet.data.length != 0) {
        freeRastaByteArray(&packet.data.data);
        freeRastaByteArray(&packet.data.checksum);
    }

    return result;
}

void red_f_deferTmo(rasta_connection *h, rasta_redundancy_channel *channel) {
    // find smallest seq_pdu in defer queue
    int smallest_index = deferqueue_smallest_seqnr(&channel->defer_q);

    // set seq_rx to it
    channel->seq_rx = channel->defer_q.elements[smallest_index].packet.sequence_number;

    logger_log(channel->logger, LOG_LEVEL_DEBUG, "RaSTA Red f_deferTmo", "calling f_deliverDeferQueue");
    red_f_deliverDeferQueue(h, channel);
}
