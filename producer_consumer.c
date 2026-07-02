
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#define BUFFER_SIZE 5
#define NUM_ITEMS 12

static int g_buffer[BUFFER_SIZE];
static int g_count = 0;
static int g_in = 0, g_out = 0;

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_not_full = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_not_empty = PTHREAD_COND_INITIALIZER;

static void *producer(void *arg) {
    (void)arg;
    for (int i = 0; i < NUM_ITEMS; i++) {
        usleep(10000 + (rand() % 30000));

        pthread_mutex_lock(&g_mutex);
        while (g_count == BUFFER_SIZE) {
            pthread_cond_wait(&g_not_full, &g_mutex);
        }

        g_buffer[g_in] = i;
        g_in = (g_in + 1) % BUFFER_SIZE;
        g_count++;
        printf("  [Producer] produced %3d (count=%d)\n", i, g_count);

        pthread_cond_signal(&g_not_empty);
        pthread_mutex_unlock(&g_mutex);
    }
    return NULL;
}

static void *consumer(void *arg) {
    int *consumed = (int *)arg;
    for (int i = 0; i < NUM_ITEMS; i++) {
        usleep(10000 + (rand() % 50000));

        pthread_mutex_lock(&g_mutex);
        while (g_count == 0) {
            pthread_cond_wait(&g_not_empty, &g_mutex);
        }

        int item = g_buffer[g_out];
        g_out = (g_out + 1) % BUFFER_SIZE;
        g_count--;
        printf("  [Consumer] consumed %3d (count=%d)\n", item, g_count);

        pthread_cond_signal(&g_not_full);
        pthread_mutex_unlock(&g_mutex);

        (*consumed)++;
    }
    return NULL;
}

int main(void) {
    printf("=== Producer-Consumer Demo (Mutex + Condition Variable, C) ===\n");

    pthread_t prod_t, cons_t;
    int consumed = 0;

    pthread_create(&prod_t, NULL, producer, NULL);
    pthread_create(&cons_t, NULL, consumer, &consumed);

    pthread_join(prod_t, NULL);
    pthread_join(cons_t, NULL);

    printf("Total items consumed: %d / %d\n", consumed, NUM_ITEMS);

    pthread_mutex_destroy(&g_mutex);
    pthread_cond_destroy(&g_not_full);
    pthread_cond_destroy(&g_not_empty);
    return 0;
}
