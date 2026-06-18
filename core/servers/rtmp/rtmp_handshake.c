#include "core/servers/rtmp/rtmp_handshake.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int read_full(int fd, void *buf, size_t len) {
    uint8_t *ptr = (uint8_t *)buf;
    size_t offset = 0;
    while (offset < len) {
        ssize_t n = recv(fd, ptr + offset, len - offset, 0);
        if (n == 0) {
            return -1;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        offset += (size_t)n;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t len) {
    const uint8_t *ptr = (const uint8_t *)buf;
    size_t offset = 0;
    while (offset < len) {
        ssize_t n = send(fd, ptr + offset, len - offset, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        offset += (size_t)n;
    }
    return 0;
}

int irs3_rtmp_perform_handshake(int fd) {
    uint8_t c0;
    uint8_t c1[1536];
    uint8_t s0 = 0x03;
    uint8_t s1[1536];
    uint8_t s2[1536];
    uint8_t c2[1536];

    if (read_full(fd, &c0, sizeof(c0)) != 0) {
        return -1;
    }
    if (c0 != 0x03) {
        return -1;
    }
    if (read_full(fd, c1, sizeof(c1)) != 0) {
        return -1;
    }

    memset(s1, 0, sizeof(s1));
    s1[0] = 0;
    s1[1] = 0;
    s1[2] = 0;
    s1[3] = 0;
    s1[4] = 0;
    s1[5] = 0;
    s1[6] = 0;
    s1[7] = 0;
    for (size_t i = 8; i < sizeof(s1); ++i) {
        s1[i] = (uint8_t)(rand() & 0xff);
    }
    memcpy(s2, c1, sizeof(s2));

    if (write_full(fd, &s0, sizeof(s0)) != 0) {
        return -1;
    }
    if (write_full(fd, s1, sizeof(s1)) != 0) {
        return -1;
    }
    if (write_full(fd, s2, sizeof(s2)) != 0) {
        return -1;
    }
    if (read_full(fd, c2, sizeof(c2)) != 0) {
        return -1;
    }
    return 0;
}
