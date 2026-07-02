
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char name[8];
    int balance;
    pthread_mutex_t lock;
} Account;

typedef struct {
    Account *src;
    Account *dst;
    int amount;
} TransferArgs;

static void add_timeout(struct timespec *ts, long ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_nsec += (ms % 1000) * 1000000L;
    ts->tv_sec += ms / 1000 + (ts->tv_nsec / 1000000000L);
    ts->tv_nsec %= 1000000000L;
}

static void *unsafe_transfer(void *arg) {
    TransferArgs *t = (TransferArgs *)arg;
    struct timespec ts;

    add_timeout(&ts, 1000);
    if (pthread_mutex_timedlock(&t->src->lock, &ts) != 0) {
        printf("  [UNSAFE] %s->%s FAILED (timeout, likely deadlock)\n", t->src->name, t->dst->name);
        return NULL;
    }
    usleep(50000);

    add_timeout(&ts, 1000);
    if (pthread_mutex_timedlock(&t->dst->lock, &ts) != 0) {
        printf("  [UNSAFE] %s->%s FAILED (timeout, likely deadlock)\n", t->src->name, t->dst->name);
        pthread_mutex_unlock(&t->src->lock);
        return NULL;
    }

    t->src->balance -= t->amount;
    t->dst->balance += t->amount;
    printf("  [UNSAFE] %s->%s transferred %d\n", t->src->name, t->dst->name, t->amount);

    pthread_mutex_unlock(&t->dst->lock);
    pthread_mutex_unlock(&t->src->lock);
    return NULL;
}

static void account_init(Account *a, const char *name, int balance) {
    snprintf(a->name, sizeof(a->name), "%s", name);
    a->balance = balance;
    pthread_mutex_init(&a->lock, NULL);
}

int main(void) {
    printf("=== Deadlock Demo: UNSAFE transfer (opposite lock order) ===\n");

    Account a, b;
    account_init(&a, "A", 1000);
    account_init(&b, "B", 1000);

    TransferArgs t1 = { &a, &b, 100 };
    TransferArgs t2 = { &b, &a, 50 };

    pthread_t th1, th2;
    pthread_create(&th1, NULL, unsafe_transfer, &t1);
    pthread_create(&th2, NULL, unsafe_transfer, &t2);
    pthread_join(th1, NULL);
    pthread_join(th2, NULL);

    printf("Final balances: A=%d B=%d\n", a.balance, b.balance);

    pthread_mutex_destroy(&a.lock);
    pthread_mutex_destroy(&b.lock);
    return 0;
}
