#include <stdio.h>
#include <string.h>

#include <teleop/packetizer.h>
#include <teleop/protocol.h>

static void print_usage(const char *program)
{
    printf("Usage: %s [--help]\n", program);
    puts("Aura native teleoperation sender (phase A scaffold).");
}

int main(int argc, char *argv[])
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    if (argc != 1) {
        print_usage(argv[0]);
        return 2;
    }

    printf("aura-sender ready: protocol=%u header=%zu bytes\n",
           TELEOP_PROTOCOL_VERSION, teleop_packet_header_size());
    return 0;
}
