#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>

/* Each thread runs this function. arg is used as the thread's ID number
   so we can tell threads apart in the output. */
void *newfunc(void *arg) {
    int id = *(int *)arg;
    for (int i = 0; i < 5; i++) {
        printf("I am thread %d, count %d\n", id, i);
        sleep(1);
    }
    return NULL;
}

int main() {
    pthread_t thread1, thread2, thread3;
    int id1 = 1, id2 = 2, id3 = 3;

    /* create 3 threads, each running newfunc concurrently */
    pthread_create(&thread1, NULL, newfunc, &id1);
    pthread_create(&thread2, NULL, newfunc, &id2);
    pthread_create(&thread3, NULL, newfunc, &id3);

    for (int i = 0; i < 5; i++) {
        printf("main thread\n");
        sleep(2);
    }

    /* wait for all 3 threads to finish before exiting */
    pthread_join(thread1, NULL);
    pthread_join(thread2, NULL);
    pthread_join(thread3, NULL);

    printf("All threads finished\n");
    return 0;
}
