#ifndef TELEOP_PROTOCOL_H
#define TELEOP_PROTOCOL_H

#include <stdint.h>

#define TELEOP_PROTOCOL_MAGIC 0x41555241U /* "AURA" */
#define TELEOP_PROTOCOL_VERSION 1U

struct teleop_packet_header {
    uint32_t magic;
    uint16_t version;
    uint16_t stream_id;
    uint32_t frame_id;
    uint16_t packet_id;
    uint16_t packet_count;
    uint32_t payload_size;
};

#endif
