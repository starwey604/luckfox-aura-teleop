#ifndef TELEOP_STATISTICS_H
#define TELEOP_STATISTICS_H

#include <stdint.h>

struct teleop_statistics {
    uint64_t packets_sent;
    uint64_t bytes_sent;
};

#endif
