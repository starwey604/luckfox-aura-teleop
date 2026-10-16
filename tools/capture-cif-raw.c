/* Bounded, standalone CIF RAW10 capture, without starting ISP/MPP.
 * SDK build: gcc -O2 -Wall -Wextra -Wpedantic -Wconversion -Werror
 *   -idirafter <kernel>/include/uapi capture-cif-raw.c -o capture-cif-raw
 * Requires a live independent USB kernel log before use. Does not configure sensor
 * controls. Restores the CIF memory mode and capture format before releasing lock.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <linux/rkcif-config.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;
static void stop(int sig) { (void)sig; interrupted = 1; }

int main(int argc, char **argv)
{
    char *end;
    long count = argc == 4 ? strtol(argv[3], &end, 10) : 0;
    if (argc != 4 || *end || count < 1 || count > 120) {
        fprintf(stderr, "usage: %s /dev/video<CIF> output.raw frames(1..120)\n", argv[0]);
        return 2;
    }
    int result = 1, streaming = 0, changed = 0;
    int fd = -1, lock = -1, original_mode = -1;
    FILE *output = NULL;
    void *maps[4] = {0};
    size_t lengths[4] = {0};
    struct v4l2_format original = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    struct v4l2_format format;
    struct v4l2_requestbuffers request = {.count = 4,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP};
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    lock = open("/tmp/teleop-mpp.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) {
        fprintf(stderr, "Camera application lock is busy/unavailable\n");
        goto cleanup;
    }
    fd = open(argv[1], O_RDWR | O_NONBLOCK | O_CLOEXEC);
    struct v4l2_capability cap = {0};
    if (fd < 0 || ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0 ||
        strncmp((const char *)cap.driver, "rkcif", 5) != 0 ||
        ioctl(fd, VIDIOC_G_FMT, &original) < 0 ||
        ioctl(fd, RKCIF_CMD_GET_CSI_MEMORY_MODE, &original_mode) < 0) {
        fprintf(stderr, "Expected standalone rkcif capture node: %s\n", strerror(errno));
        goto cleanup;
    }
    if (original.fmt.pix_mp.width != 1280 || original.fmt.pix_mp.height != 720 ||
        original.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_SGRBG10) {
        fprintf(stderr, "Expected preconfigured 1280x720 BA10\n");
        goto cleanup;
    }
    /* Exclusive create prevents accidental overwriting of earlier raw samples. */
    int out_fd = open(argv[2], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (out_fd < 0) { perror("create raw"); goto cleanup; }
    output = fdopen(out_fd, "wb");
    if (!output) { close(out_fd); goto cleanup; }
    int mode = CSI_LVDS_MEM_WORD_LOW_ALIGN;
    if (ioctl(fd, RKCIF_CMD_SET_CSI_MEMORY_MODE, &mode) < 0) {
        perror("set RAW low align"); goto cleanup;
    }
    changed = 1;
    format = original;
    format.fmt.pix_mp.plane_fmt[0].bytesperline = 0;
    format.fmt.pix_mp.plane_fmt[0].sizeimage = 0;
    if (ioctl(fd, VIDIOC_S_FMT, &format) < 0 ||
        ioctl(fd, RKCIF_CMD_GET_CSI_MEMORY_MODE, &mode) < 0 || mode != CSI_LVDS_MEM_WORD_LOW_ALIGN ||
        format.fmt.pix_mp.num_planes != 1 || format.fmt.pix_mp.plane_fmt[0].bytesperline < 2560) {
        fprintf(stderr, "Could not obtain unpacked low-aligned RAW10\n"); goto cleanup;
    }
    printf("{\"width\":%u,\"height\":%u,\"stride\":%u,\"sizeimage\":%u,"
           "\"memory_mode\":%d,\"original_memory_mode\":%d,\"format\":\"GRBG10_LE16\"}\n",
           format.fmt.pix_mp.width, format.fmt.pix_mp.height,
           format.fmt.pix_mp.plane_fmt[0].bytesperline, format.fmt.pix_mp.plane_fmt[0].sizeimage,
           mode, original_mode);
    fflush(stdout);
    if (ioctl(fd, VIDIOC_REQBUFS, &request) < 0 || request.count < 2 || request.count > 4) {
        perror("request raw buffers"); goto cleanup;
    }
    for (unsigned i = 0; i < request.count; ++i) {
        struct v4l2_plane plane = {0};
        struct v4l2_buffer buffer = {.type = type, .memory = V4L2_MEMORY_MMAP,
            .index = i, .length = 1, .m.planes = &plane};
        if (ioctl(fd, VIDIOC_QUERYBUF, &buffer) < 0) { perror("query raw buffer"); goto cleanup; }
        maps[i] = mmap(NULL, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, plane.m.mem_offset);
        if (maps[i] == MAP_FAILED) { maps[i] = NULL; perror("mmap"); goto cleanup; }
        lengths[i] = plane.length;
        if (ioctl(fd, VIDIOC_QBUF, &buffer) < 0) { perror("queue raw buffer"); goto cleanup; }
    }
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) { perror("start raw"); goto cleanup; }
    streaming = 1;
    for (long i = 0; i < count && !interrupted;) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int ready = poll(&pfd, 1, 2000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) { fprintf(stderr, "RAW frame timeout/error\n"); goto cleanup; }
        struct v4l2_plane plane = {0};
        struct v4l2_buffer buffer = {.type = type, .memory = V4L2_MEMORY_MMAP,
            .length = 1, .m.planes = &plane};
        if (ioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            perror("dequeue raw"); goto cleanup;
        }
        if (buffer.index >= request.count || plane.bytesused > lengths[buffer.index] ||
            plane.bytesused < plane.data_offset ||
            plane.bytesused - plane.data_offset != format.fmt.pix_mp.plane_fmt[0].sizeimage ||
            (buffer.flags & V4L2_BUF_FLAG_ERROR)) {
            fprintf(stderr, "Unexpected RAW buffer size/flags\n"); goto cleanup;
        }
        size_t bytes = plane.bytesused - plane.data_offset;
        if (fwrite((char *)maps[buffer.index] + plane.data_offset, 1, bytes, output) != bytes) {
            perror("write raw"); goto cleanup;
        }
        printf("{\"frame\":%ld,\"sequence\":%u,\"bytes\":%zu,\"timestamp_us\":%lld}\n",
               i, buffer.sequence, bytes, (long long)buffer.timestamp.tv_sec * 1000000 + buffer.timestamp.tv_usec);
        fflush(stdout);
        ++i;
        if (ioctl(fd, VIDIOC_QBUF, &buffer) < 0) { perror("requeue raw"); goto cleanup; }
    }
    result = interrupted ? 1 : 0;
cleanup:
    if (streaming && ioctl(fd, VIDIOC_STREAMOFF, &type) < 0) { perror("stop raw"); result = 1; }
    for (unsigned i = 0; i < 4; ++i)
        if (maps[i]) munmap(maps[i], lengths[i]);
    if (fd >= 0 && changed) {
        request.count = 0;
        if (ioctl(fd, VIDIOC_REQBUFS, &request) < 0 ||
            ioctl(fd, RKCIF_CMD_SET_CSI_MEMORY_MODE, &original_mode) < 0 ||
            ioctl(fd, VIDIOC_S_FMT, &original) < 0) {
            perror("restore CIF capture configuration"); result = 1;
        }
    }
    if (output && fclose(output) != 0) { perror("close raw"); result = 1; }
    if (fd >= 0) close(fd);
    if (lock >= 0) close(lock);
    return result;
}
