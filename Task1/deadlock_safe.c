#include <stdio.h>

int main() {
    int alloc[3] = {1, 0, 2}; // Allocations for P0, P1, P2
    int max[3] = {3, 2, 2};   // Maximum demands
    int need[3];
    int available = 2;        // Unallocated system assets
    int finished[3] = {0, 0, 0}, safeSequence[3], seqCount = 0;

    printf("--- Banker's Algorithm: Safe Sequence Test ---\n");
    for (int i = 0; i < 3; i++) need[i] = max[i] - alloc[i];

    for (int step = 0; step < 3; step++) {
        for (int i = 0; i < 3; i++) {
            if (finished[i] == 0 && need[i] <= available) {
                available += alloc[i];
                finished[i] = 1;
                safeSequence[seqCount++] = i;
                break;
            }
        }
    }

    if (seqCount == 3) {
        printf("System is in a SAFE state.\nSequence: ");
        for (int i = 0; i < 3; i++) printf("P%d ", safeSequence[i]);
        printf("\n");
    } else {
        printf("Error: System is UNSAFE (Deadlock risk).\n");
    }
    return 0;
}
