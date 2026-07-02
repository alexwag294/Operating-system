#include <stdio.h>
#include <pthread.h>

int counter = 0;  // Shared variable (lives in Data segment)
pthread_mutex_t lock;

/* UNSAFE: no lock, counter++ is not atomic -> race condition */
void *increment_unsafe(void *arg) {
    for (int i = 0; i < 100000; i++) {
        counter++;
    }
    return NULL;
}

/* SAFE: protected by a mutex -> no race condition */
void *increment_safe(void *arg) {
    for (int i = 0; i < 100000; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

int main() {
    pthread_t t1, t2;

    // ---- UNSAFE version ----
    counter = 0;
    pthread_create(&t1, NULL, increment_unsafe, NULL);
    pthread_create(&t2, NULL, increment_unsafe, NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    printf("UNSAFE  -> Expected: 200000, Actual: %d\n", counter);

    // ---- SAFE version (with mutex) ----
    counter = 0;
    pthread_mutex_init(&lock, NULL);

    pthread_create(&t1, NULL, increment_safe, NULL);
    pthread_create(&t2, NULL, increment_safe, NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    printf("SAFE    -> Expected: 200000, Actual: %d\n", counter);

    pthread_mutex_destroy(&lock);
    return 0;
}
