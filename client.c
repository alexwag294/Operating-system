/*
 * client.c
 * --------
 * Interactive client for the authenticated key-value server.
 * Standalone version: shared helpers (line framing) duplicated here
 * instead of a separate common.h/common.c.
 *
 * Usage: ./client [host] [port]
 * Build: gcc -Wall -Wextra -O2 -pthread -o client client.c
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_PORT           5050
#define MAX_LINE               2048
#define MAX_USERNAME_LEN         64
#define MAX_PASSWORD_LEN        256
#define RECV_TIMEOUT_SECONDS     10

/* ---------------------------------------------------------------------
 * Shared line-framing helpers (duplicated in server.c)
 * --------------------------------------------------------------------- */

static ssize_t read_line(int fd, char *buf, size_t buflen) {
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

static int send_line(int fd, const char *fmt, ...) {
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

/* ---------------------------------------------------------------------
 * Client logic
 * --------------------------------------------------------------------- */

static int connect_to_server(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid host address: %s\n", host);
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Could not connect to %s:%d -> %s\n", host, port, strerror(errno));
        close(fd);
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = RECV_TIMEOUT_SECONDS;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return fd;
}

static int print_response(int fd) {
    char line[MAX_LINE];
    ssize_t n = read_line(fd, line, sizeof(line));

    if (n == 0) {
        printf("[client] Server closed the connection.\n");
        return -1;
    }
    if (n == -2) {
        printf("[client] Server sent an oversized line (protocol error).\n");
        return -1;
    }
    if (n < 0) {
        printf("[client] Timed out or lost connection waiting for a response.\n");
        return -1;
    }

    printf("  -> %s\n", line);
    return 0;
}

static int authenticate(int fd, const char *username, const char *password) {
    if (send_line(fd, "AUTH %s %s", username, password) < 0) {
        printf("[client] Failed to send AUTH (connection lost).\n");
        return -1;
    }

    char line[MAX_LINE];
    ssize_t n = read_line(fd, line, sizeof(line));
    if (n <= 0) {
        printf("[client] Lost connection during authentication.\n");
        return -1;
    }

    if (strncmp(line, "AUTH_OK", 7) == 0) {
        printf("[client] Authenticated. %s\n", line);
        return 0;
    } else {
        printf("[client] Authentication rejected: %s\n", line);
        return -1;
    }
}

int main(int argc, char *argv[]) {
    const char *host = (argc >= 2) ? argv[1] : "127.0.0.1";
    int port = (argc >= 3) ? atoi(argv[2]) : DEFAULT_PORT;

    signal(SIGPIPE, SIG_IGN);

    int fd = connect_to_server(host, port);
    if (fd < 0) {
        return 1;
    }

    char username[MAX_USERNAME_LEN + 1];
    char password[MAX_PASSWORD_LEN + 1];

    printf("Username: ");
    if (!fgets(username, sizeof(username), stdin)) {
        close(fd);
        return 1;
    }
    username[strcspn(username, "\n")] = '\0';

    printf("Password: ");
    if (!fgets(password, sizeof(password), stdin)) {
        close(fd);
        return 1;
    }
    password[strcspn(password, "\n")] = '\0';

    if (authenticate(fd, username, password) < 0) {
        close(fd);
        return 1;
    }

    printf("Connected. Commands: SET <key> <value> | GET <key> | DEL <key> | LIST | PING | QUIT\n");

    char input[MAX_LINE];
    for (;;) {
        printf("kv> ");
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin)) {
            send_line(fd, "QUIT");
            print_response(fd);
            break;
        }
        input[strcspn(input, "\n")] = '\0';

        if (strlen(input) == 0) {
            continue;
        }

        if (send_line(fd, "%s", input) < 0) {
            printf("[client] Connection lost while sending command.\n");
            break;
        }

        int is_quit = (strncmp(input, "QUIT", 4) == 0);
        int is_list = (strncmp(input, "LIST", 4) == 0);

        if (is_list) {
            for (;;) {
                char line[MAX_LINE];
                ssize_t n = read_line(fd, line, sizeof(line));
                if (n <= 0) {
                    printf("[client] Connection lost while reading LIST response.\n");
                    close(fd);
                    return 1;
                }
                printf("  -> %s\n", line);
                if (strcmp(line, "LIST_END") == 0 || strncmp(line, "ERR", 3) == 0) {
                    break;
                }
            }
        } else {
            if (print_response(fd) < 0) {
                break;
            }
        }

        if (is_quit) {
            break;
        }
    }

    close(fd);
    return 0;
}
