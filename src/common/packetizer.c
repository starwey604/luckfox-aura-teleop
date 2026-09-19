#include <teleop/packetizer.h>
#include <teleop/protocol.h>

size_t teleop_packet_header_size(void)
{
    return sizeof(struct teleop_packet_header);
}
