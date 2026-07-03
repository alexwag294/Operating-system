#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

pthread_mutex_t lock;   // mutex protecting shared_counter
int shared_counter = 0;

void *worker(void *arg) {
    int id = *(int *)arg;

    for (int i = 0; i < 3; i++) {
        pthread_mutex_lock(&lock);      // enter critical section

        int temp = shared_counter;
        printf("Thread %d entering critical section, counter=%d\n", id, temp);
        usleep(100000); // simulate work while holding the mutex
        shared_counter = temp + 1;
        printf("Thread %d leaving critical section, counter=%d\n", id, shared_counter);

        pthread_mutex_unlock(&lock);    // exit critical section
        usleep(50000);
    }
    return NULL;
}

int main() {
    pthread_t t1, t2, t3;
    int id1 = 1, id2 = 2, id3 = 3;

    pthread_mutex_init(&lock, NULL);

    pthread_create(&t1, NULL, worker, &id1);
    pthread_create(&t2, NULL, worker, &id2);
    pthread_create(&t3, NULL, worker, &id3);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);

    printf("Final counter value: %d (expected 9)\n", shared_counter);

    pthread_mutex_destroy(&lock);
    return 0;
}
