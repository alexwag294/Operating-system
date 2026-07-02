
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#define TIME_QUANTUM_MS 50
#define SLEEP_SCALE 20
#define MAX_PROCS 8

typedef struct {
    int pid;
    int burst_ms;
    int remaining_ms;
    int completion_ms;
} Process;

static pthread_mutex_t g_cpu_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_clock_ms = 0;

typedef struct {
    Process *proc;
    int slice_ms;
} SliceArg;

static void *run_slice(void *arg) {
    SliceArg *sa = (SliceArg *)arg;
    pthread_mutex_lock(&g_cpu_lock);
    printf("  t=%4dms  PID %d running for %dms (remaining before=%dms)\n",
           g_clock_ms, sa->proc->pid, sa->slice_ms, sa->proc->remaining_ms);
    usleep((sa->slice_ms * 1000) / SLEEP_SCALE);
    pthread_mutex_unlock(&g_cpu_lock);
    return NULL;
}

int main(void) {
    printf("=== Round Robin Scheduler Simulation (C) ===\n");

    Process procs[] = {
        {1, 120, 120, -1},
        {2, 200, 200, -1},
        {3, 70,  70,  -1},
        {4, 150, 150, -1},
    };
    int n = sizeof(procs) / sizeof(procs[0]);

    int queue[MAX_PROCS * 4];
    int qhead = 0, qtail = 0, qlen = 0;
    for (int i = 0; i < n; i++) { queue[qtail++] = i; qlen++; }

    int gantt_pid[256], gantt_ms[256], gantt_len = 0;
    int completed = 0;

    while (qlen > 0) {
        int idx = queue[qhead];
        qhead = (qhead + 1) % (MAX_PROCS * 4);
        qlen--;

        Process *p = &procs[idx];
        int slice = p->remaining_ms < TIME_QUANTUM_MS ? p->remaining_ms : TIME_QUANTUM_MS;

        SliceArg sa = { p, slice };
        pthread_t t;
        pthread_create(&t, NULL, run_slice, &sa);
        pthread_join(t, NULL);

        p->remaining_ms -= slice;
        g_clock_ms += slice;
        gantt_pid[gantt_len] = p->pid;
        gantt_ms[gantt_len] = slice;
        gantt_len++;

        if (p->remaining_ms > 0) {
            queue[qtail] = idx;
            qtail = (qtail + 1) % (MAX_PROCS * 4);
            qlen++;
        } else {
            p->completion_ms = g_clock_ms;
            completed++;
            printf("  --> PID %d COMPLETED at t=%dms\n", p->pid, g_clock_ms);
        }
    }

    printf("\nGantt chart (pid:slice_ms):\n  ");
    for (int i = 0; i < gantt_len; i++) {
        printf("P%d:%dms%s", gantt_pid[i], gantt_ms[i], (i == gantt_len - 1) ? "\n" : " | ");
    }
    printf("Processes completed: %d/%d\n", completed, n);
    return 0;
}
