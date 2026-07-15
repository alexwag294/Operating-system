/*
 * server.c
 * --------
 * Multi-threaded TCP server exposing a small authenticated key-value store.
 * Standalone version: all shared helpers (line framing, hashing) are
 * defined directly in this file instead of a separate common.h/common.c,
 * so only server.c and client.c need to exist.
 *
 * Build: gcc -Wall -Wextra -O2 -pthread -o server server.c
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------------
 * Protocol / limit constants
 * --------------------------------------------------------------------- */
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

/* ---------------------------------------------------------------------
 * Shared line-framing helpers (duplicated in client.c)
 * --------------------------------------------------------------------- */

/* Reads one '\n'-terminated line from fd into buf (size buflen, including
 * room for the NUL terminator). Returns >0 length, 0 on clean EOF,
 * -1 on socket error/timeout, -2 if the line exceeded buflen. */
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

    /* Oversized line: drain until newline or disconnect, then reject. */
    char discard;
    ssize_t n;
    do {
        n = recv(fd, &discard, 1, 0);
    } while (n > 0 && discard != '\n');

    return -2;
}

/* Sends "fmt..." followed by '\n'. Returns 0 on success, -1 on error. */
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

/* Non-cryptographic demo hash (djb2-style, salted). NOT for production -
 * see README caveat: a real system needs bcrypt/argon2/PBKDF2. */
static unsigned long simple_hash(const char *salt, const char *input) {
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

/* ---------------------------------------------------------------------
 * "User database" - hardcoded for this demo.
 * --------------------------------------------------------------------- */
#define PASSWORD_SALT "static-demo-salt"

typedef struct {
    const char *username;
    unsigned long password_hash;
} user_record_t;

static user_record_t USER_DB[] = {
    {"alice", 0},
    {"bob",   0},
};
#define USER_DB_LEN (sizeof(USER_DB) / sizeof(USER_DB[0]))

static void init_user_db(void) {
    USER_DB[0].password_hash = simple_hash(PASSWORD_SALT, "wonderland");
    USER_DB[1].password_hash = simple_hash(PASSWORD_SALT, "builder123");
}

/* ---------------------------------------------------------------------
 * Thread-safe in-memory key-value store (linked list + mutex)
 * --------------------------------------------------------------------- */
typedef struct kv_node {
    char key[MAX_KEY_LEN + 1];
    char value[MAX_VALUE_LEN + 1];
    struct kv_node *next;
} kv_node_t;

static kv_node_t *g_store_head = NULL;
static pthread_mutex_t g_store_lock = PTHREAD_MUTEX_INITIALIZER;

static void store_set(const char *key, const char *value) {
    pthread_mutex_lock(&g_store_lock);
    for (kv_node_t *n = g_store_head; n; n = n->next) {
        if (strcmp(n->key, key) == 0) {
            strncpy(n->value, value, MAX_VALUE_LEN);
            n->value[MAX_VALUE_LEN] = '\0';
            pthread_mutex_unlock(&g_store_lock);
            return;
        }
    }
    kv_node_t *node = malloc(sizeof(kv_node_t));
    if (!node) {
        pthread_mutex_unlock(&g_store_lock);
        return;
    }
    strncpy(node->key, key, MAX_KEY_LEN);
    node->key[MAX_KEY_LEN] = '\0';
    strncpy(node->value, value, MAX_VALUE_LEN);
    node->value[MAX_VALUE_LEN] = '\0';
    node->next = g_store_head;
    g_store_head = node;
    pthread_mutex_unlock(&g_store_lock);
}

static int store_get(const char *key, char *out_value) {
    int found = 0;
    pthread_mutex_lock(&g_store_lock);
    for (kv_node_t *n = g_store_head; n; n = n->next) {
        if (strcmp(n->key, key) == 0) {
            strcpy(out_value, n->value);
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_store_lock);
    return found;
}

static int store_delete(const char *key) {
    int found = 0;
    pthread_mutex_lock(&g_store_lock);
    kv_node_t *prev = NULL, *n = g_store_head;
    while (n) {
        if (strcmp(n->key, key) == 0) {
            if (prev) prev->next = n->next;
            else g_store_head = n->next;
            free(n);
            found = 1;
            break;
        }
        prev = n;
        n = n->next;
    }
    pthread_mutex_unlock(&g_store_lock);
    return found;
}

static int store_list(char out[][MAX_KEY_LEN + 1], int max_keys) {
    int count = 0;
    pthread_mutex_lock(&g_store_lock);
    for (kv_node_t *n = g_store_head; n && count < max_keys; n = n->next) {
        strcpy(out[count], n->key);
        count++;
    }
    pthread_mutex_unlock(&g_store_lock);
    return count;
}

/* ---------------------------------------------------------------------
 * Connection accounting
 * --------------------------------------------------------------------- */
static sem_t g_conn_slots;

typedef struct {
    int fd;
    struct sockaddr_in addr;
} client_ctx_t;

static void log_msg(const char *level, struct sockaddr_in *addr, const char *fmt, ...) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char timestr[32];
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", &tm_now);

    fprintf(stderr, "%s [%s] %s:%d - ", timestr, level, ip, ntohs(addr->sin_port));

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

/* Splits a line into up to max_parts tokens; the LAST part gets the
 * remainder of the line (so values may contain spaces). */
static int split_line(char *line, char *parts[], int max_parts) {
    int count = 0;
    char *p = line;

    while (count < max_parts - 1) {
        while (*p == ' ') p++;
        if (*p == '\0') break;
        parts[count++] = p;
        while (*p && *p != ' ') p++;
        if (*p == ' ') {
            *p = '\0';
            p++;
        }
    }
    while (*p == ' ') p++;
    if (*p != '\0' && count < max_parts) {
        parts[count++] = p;
    }
    return count;
}

static int handle_authentication(int fd, struct sockaddr_in *addr, char *out_username) {
    char line[MAX_LINE];

    for (int attempt = 0; attempt < MAX_AUTH_ATTEMPTS; attempt++) {
        ssize_t n = read_line(fd, line, sizeof(line));
        if (n == 0) {
            log_msg("INFO", addr, "client disconnected during auth");
            return 0;
        }
        if (n == -2) {
            send_line(fd, "ERR BAD_REQUEST line too long");
            continue;
        }
        if (n < 0) {
            log_msg("INFO", addr, "socket error/timeout during auth");
            return 0;
        }

        char *parts[3];
        int nparts = split_line(line, parts, 3);

        if (nparts != 3 || strcmp(parts[0], "AUTH") != 0) {
            send_line(fd, "ERR AUTH_REQUIRED you must authenticate first");
            continue;
        }

        const char *username = parts[1];
        const char *password = parts[2];

        if (strlen(username) == 0 || strlen(username) > MAX_USERNAME_LEN ||
            strlen(password) == 0 || strlen(password) > MAX_PASSWORD_LEN) {
            send_line(fd, "ERR BAD_REQUEST invalid username/password length");
            continue;
        }

        unsigned long supplied_hash = simple_hash(PASSWORD_SALT, password);
        int ok = 0;
        for (size_t i = 0; i < USER_DB_LEN; i++) {
            if (strcmp(USER_DB[i].username, username) == 0 &&
                USER_DB[i].password_hash == supplied_hash) {
                ok = 1;
                break;
            }
        }

        if (ok) {
            strncpy(out_username, username, MAX_USERNAME_LEN);
            out_username[MAX_USERNAME_LEN] = '\0';
            send_line(fd, "AUTH_OK %lx%lx", (unsigned long)time(NULL), simple_hash(username, password));
            log_msg("INFO", addr, "user '%s' authenticated", username);
            return 1;
        } else {
            log_msg("WARN", addr, "failed auth attempt %d/%d for user '%s'",
                    attempt + 1, MAX_AUTH_ATTEMPTS, username);
            send_line(fd, "AUTH_FAIL invalid credentials");
        }
    }

    send_line(fd, "ERR AUTH_LOCKED too many failed attempts");
    log_msg("WARN", addr, "connection dropped: too many failed auth attempts");
    return 0;
}

static int valid_key(const char *key) {
    size_t len = strlen(key);
    return (len >= 1 && len <= MAX_KEY_LEN);
}

static void serve_commands(int fd, struct sockaddr_in *addr, const char *username) {
    char line[MAX_LINE];

    for (;;) {
        ssize_t n = read_line(fd, line, sizeof(line));

        if (n == 0) {
            log_msg("INFO", addr, "client disconnected");
            return;
        }
        if (n == -2) {
            send_line(fd, "ERR BAD_REQUEST line too long");
            continue;
        }
        if (n < 0) {
            log_msg("INFO", addr, "socket error/timeout, closing");
            return;
        }

        char *parts[3];
        int nparts = split_line(line, parts, 3);
        if (nparts == 0) {
            continue;
        }
        const char *cmd = parts[0];

        if (strcmp(cmd, "QUIT") == 0) {
            send_line(fd, "BYE");
            return;

        } else if (strcmp(cmd, "PING") == 0) {
            send_line(fd, "PONG");

        } else if (strcmp(cmd, "SET") == 0) {
            if (nparts != 3) {
                send_line(fd, "ERR BAD_REQUEST usage: SET <key> <value>");
                continue;
            }
            if (!valid_key(parts[1])) {
                send_line(fd, "ERR BAD_REQUEST key length invalid (1-%d)", MAX_KEY_LEN);
                continue;
            }
            if (strlen(parts[2]) > MAX_VALUE_LEN) {
                send_line(fd, "ERR BAD_REQUEST value too long (max %d)", MAX_VALUE_LEN);
                continue;
            }
            store_set(parts[1], parts[2]);
            send_line(fd, "OK set '%s'", parts[1]);
            log_msg("INFO", addr, "%s SET %s", username, parts[1]);

        } else if (strcmp(cmd, "GET") == 0) {
            if (nparts != 2 || !valid_key(parts[1])) {
                send_line(fd, "ERR BAD_REQUEST usage: GET <key>");
                continue;
            }
            char value[MAX_VALUE_LEN + 1];
            if (store_get(parts[1], value)) {
                send_line(fd, "VALUE %s %s", parts[1], value);
            } else {
                send_line(fd, "NOT_FOUND %s", parts[1]);
            }

        } else if (strcmp(cmd, "DEL") == 0) {
            if (nparts != 2 || !valid_key(parts[1])) {
                send_line(fd, "ERR BAD_REQUEST usage: DEL <key>");
                continue;
            }
            if (store_delete(parts[1])) {
                send_line(fd, "OK deleted '%s'", parts[1]);
            } else {
                send_line(fd, "NOT_FOUND %s", parts[1]);
            }

        } else if (strcmp(cmd, "LIST") == 0) {
            char keys[256][MAX_KEY_LEN + 1];
            int count = store_list(keys, 256);
            send_line(fd, "LIST_BEGIN");
            for (int i = 0; i < count; i++) {
                send_line(fd, "KEY %s", keys[i]);
            }
            send_line(fd, "LIST_END");

        } else {
            send_line(fd, "ERR UNKNOWN_TYPE unrecognized command '%s'", cmd);
        }
    }
}

static void *client_thread(void *arg) {
    client_ctx_t *ctx = (client_ctx_t *)arg;
    int fd = ctx->fd;
    struct sockaddr_in addr = ctx->addr;
    free(ctx);

    struct timeval tv;
    tv.tv_sec = IDLE_TIMEOUT_SECONDS;
    tv.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        log_msg("WARN", &addr, "setsockopt(SO_RCVTIMEO) failed: %s", strerror(errno));
    }

    log_msg("INFO", &addr, "connection opened");

    char username[MAX_USERNAME_LEN + 1] = {0};
    if (handle_authentication(fd, &addr, username)) {
        serve_commands(fd, &addr, username);
    }

    close(fd);
    log_msg("INFO", &addr, "connection closed");
    sem_post(&g_conn_slots);
    return NULL;
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    if (argc >= 2) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Invalid port: %s\n", argv[1]);
            return 1;
        }
    }

    signal(SIGPIPE, SIG_IGN);

    init_user_db();
    sem_init(&g_conn_slots, 0, MAX_CLIENTS);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, BACKLOG) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    fprintf(stderr, "Server listening on port %d\n", port);

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            continue;
        }

        if (sem_trywait(&g_conn_slots) != 0) {
            log_msg("WARN", &client_addr, "connection limit reached, rejecting");
            send_line(client_fd, "ERR SERVER_BUSY too many connections, try again later");
            close(client_fd);
            continue;
        }

        client_ctx_t *ctx = malloc(sizeof(client_ctx_t));
        if (!ctx) {
            fprintf(stderr, "malloc failed, dropping connection\n");
            close(client_fd);
            sem_post(&g_conn_slots);
            continue;
        }
        ctx->fd = client_fd;
        ctx->addr = client_addr;

        pthread_t tid;
        int rc = pthread_create(&tid, NULL, client_thread, ctx);
        if (rc != 0) {
            fprintf(stderr, "pthread_create failed: %s\n", strerror(rc));
            close(client_fd);
            free(ctx);
            sem_post(&g_conn_slots);
            continue;
        }
        pthread_detach(tid);
    }

    close(listen_fd);
    return 0;
}
