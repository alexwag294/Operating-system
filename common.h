#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>
#include <sys/types.h>

#define DEFAULT_PORT        5050
#define BACKLOG              128
#define MAX_LINE            2048
#define MAX_KEY_LEN          128
#define MAX_VALUE_LEN       1024
#define MAX_USERNAME_LEN      64
#define MAX_PASSWORD_LEN     256
#define MAX_CLIENTS           50
#define MAX_AUTH_ATTEMPTS      3
#define IDLE_TIMEOUT_SECONDS 120

ssize_t read_line(int fd, char *buf, size_t buflen);
int send_line(int fd, const char *fmt, ...);
unsigned long simple_hash(const char *salt, const char *input);

#endif
