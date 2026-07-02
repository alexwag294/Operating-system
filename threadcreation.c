/*
 * thread_creation_demo.c
 * -------------------------
 * Requirement 1: Create multiple threads to perform concurrent tasks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#define NUM_THREADS 4
#define ARRAY_SIZE 40

typedef struct {
    int thread_index;
    const int *array;
    int start;
    int end;
    long partial_sum;
} ThreadTask;

void *sum_worker(void *arg) {
    ThreadTask *task = (ThreadTask *)arg;

    printf("  [Thread %d] started (tid=%lu), summing indices [%d, %d)\n",
           task->thread_index, (unsigned long)pthread_self(), task->start, task->end);

    long sum = 0;
    for (int i = task->start; i < task->end; i++) {
        sum += task->array[i];
        usleep(1000);
    }
    task->partial_sum = sum;

    printf("  [Thread %d] finished, partial_sum=%ld\n", task->thread_index, sum);
    return NULL;
}

int main(void) {
    printf("=== Thread Creation Demo (C / pthreads) ===\n");

    int data[ARRAY_SIZE];
    for (int i = 0; i < ARRAY_SIZE; i++) {
        data[i] = i + 1;
    }

    pthread_t threads[NUM_THREADS];
    ThreadTask tasks[NUM_THREADS];

    int chunk = ARRAY_SIZE / NUM_THREADS;

    for (int i = 0; i < NUM_THREADS; i++) {
        tasks[i].thread_index = i;
        tasks[i].array = data;
        tasks[i].start = i * chunk;
        tasks[i].end = (i == NUM_THREADS - 1) ? ARRAY_SIZE : (i + 1) * chunk;
        tasks[i].partial_sum = 0;

        int rc = pthread_create(&threads[i], NULL, sum_worker, &tasks[i]);
        if (rc != 0) {
            fprintf(stderr, "Error creating thread %d (rc=%d)\n", i, rc);
            return 1;
        }
    }

    long total = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
        total += tasks[i].partial_sum;
    }

    long expected = (long)ARRAY_SIZE * (ARRAY_SIZE + 1) / 2;
    printf("\nAll %d threads completed.\n", NUM_THREADS);
    printf("Total sum = %ld (expected %ld) %s\n",
           total, expected, (total == expected) ? "OK" : "MISMATCH");

    return 0;
}
