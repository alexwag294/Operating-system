#include <stdio.h>

int main() {
    int alloc[3] = {2, 1, 2};
    int max[3] = {5, 4, 3};    // High maximum demands
    int need[3];
    int available = 0;          // Zero unallocated system assets
    int finished[3] = {0, 0, 0}, seqCount = 0;

    printf("\n--- Banker's Algorithm: Unsafe State Test ---\n");
    for (int i = 0; i < 3; i++) need[i] = max[i] - alloc[i];

    for (int step = 0; step < 3; step++) {
        int found = 0;
        for (int i = 0; i < 3; i++) {
            if (finished[i] == 0 && need[i] <= available) {
                available += alloc[i];
                finished[i] = 1;
                seqCount++;
                found = 1;
                break;
            }
        }
        if (!found) break; // Terminate if no process can proceed
    }

    if (seqCount == 3) {
        printf("System is SAFE.\n");
    } else {
        printf("Alert: System state is UNSAFE. Resource request denied.\n");
    }
    return 0;
}
