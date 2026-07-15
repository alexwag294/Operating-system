#include "common.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

ssize_t read_line(int fd, char *buf, size_t buflen) {
    size_t total = 0;

    if (buflen == 0) {
        return -2;
    }

    while (total + 1 < buflen) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);

        if (n == 0) {
            return (total == 0) ? 0 : -1;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        if (c == '\n') {
            if (total > 0 && buf[total - 1] == '\r') {
                total--;
            }
            buf[total] = '\0';
            return (ssize_t)total;
        }

        buf[total++] = c;
    }

    char discard;
    ssize_t n;
    do {
        n = recv(fd, &discard, 1, 0);
    } while (n > 0 && discard != '\n');

    return -2;
}

int send_line(int fd, const char *fmt, ...) {
    char buf[MAX_LINE];
    va_list ap;

    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);

    if (len < 0) {
        return -1;
    }
    if ((size_t)len >= sizeof(buf) - 1) {
        len = (int)sizeof(buf) - 2;
    }

    buf[len++] = '\n';

    size_t sent = 0;
    while (sent < (size_t)len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

unsigned long simple_hash(const char *salt, const char *input) {
    unsigned long hash = 5381;
    const char *s;

    for (s = salt; *s; s++) {
        hash = ((hash << 5) + hash) + (unsigned char)(*s);
    }
    for (s = input; *s; s++) {
        hash = ((hash << 5) + hash) + (unsigned char)(*s);
    }
    return hash;
}
