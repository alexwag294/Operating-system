#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =====================================================================
   CONFIGURATION - change these to test different scenarios
===================================================================== */
#define PAGE_SIZE_BYTES   8      // simulated size of one page, in bytes
#define NUM_FRAMES        3      // number of physical memory frames
#define NUM_REFERENCES     10     // length of the page reference string

/* =====================================================================
   ANSI COLOUR CODES - for readable terminal output
===================================================================== */
#define CLR_RESET   "\033[0m"
#define CLR_BOLD    "\033[1m"
#define CLR_FAINT   "\033[2m"
#define CLR_RED     "\033[31m"
#define CLR_GREEN   "\033[32m"
#define CLR_YELLOW  "\033[33m"
#define CLR_CYAN    "\033[36m"
#define CLR_MAGENTA "\033[35m"

/* =====================================================================
   A single physical memory frame.
===================================================================== */
typedef struct {
    int  pageNumber;     // which page currently sits here (-1 = empty)
    int  arrivalTick;    // when this page was loaded (used by FIFO)
    int  touchedTick;    // when this page was last accessed (used by LRU)
    char payload[PAGE_SIZE_BYTES]; // fake "page content"
} MemoryFrame;

/* Results collected while running one algorithm. */
typedef struct {
    int hitCount;
    int faultCount;
} SimResult;

/* ---------------------------------------------------------------
   Look for pageNumber already resident in one of the frames.
   Returns the frame index, or -1 if it isn't currently loaded.
--------------------------------------------------------------- */
static int locatePage(MemoryFrame *mem, int pageNumber) {
    for (int f = 0; f < NUM_FRAMES; f++) {
        if (mem[f].pageNumber == pageNumber) return f;
    }
    return -1;
}

/* Returns the index of the first free frame, or -1 if memory is full. */
static int locateFreeFrame(MemoryFrame *mem) {
    for (int f = 0; f < NUM_FRAMES; f++) {
        if (mem[f].pageNumber == -1) return f;
    }
    return -1;
}

/* ---------------------------------------------------------------
   Copies a page's "content" into a frame (simulates the OS pulling
   the page in from disk) and stamps its arrival/touched ticks.
--------------------------------------------------------------- */
static void loadPageIntoFrame(MemoryFrame *frame, int pageNumber, int tick) {
    frame->pageNumber  = pageNumber;
    frame->arrivalTick = tick;
    frame->touchedTick = tick;
    for (int b = 0; b < PAGE_SIZE_BYTES; b++) {
        frame->payload[b] = (char)('a' + pageNumber);
    }
    printf(CLR_YELLOW "     loaded P%d  " CLR_FAINT "(payload: ", pageNumber);
    for (int b = 0; b < PAGE_SIZE_BYTES; b++) printf("%c", frame->payload[b]);
    printf(")" CLR_RESET "\n");
}

/* Prints the current state of all frames, plus whether this access
   was a HIT or a FAULT. */
static void showMemoryRow(MemoryFrame *mem, int requestedPage, int wasFault) {
    printf("  req P%-2d  ->  ", requestedPage);
    for (int f = 0; f < NUM_FRAMES; f++) {
        if (mem[f].pageNumber == -1) {
            printf(CLR_FAINT "[   ]" CLR_RESET " ");
        } else if (mem[f].pageNumber == requestedPage) {
            printf(CLR_CYAN "[ P%-2d]" CLR_RESET " ", mem[f].pageNumber);
        } else {
            printf(CLR_GREEN "[ P%-2d]" CLR_RESET " ", mem[f].pageNumber);
        }
    }
    if (wasFault) printf(CLR_RED "  FAULT" CLR_RESET "\n");
    else          printf(CLR_GREEN "  HIT" CLR_RESET "\n");
}

static void printBanner(const char *algoName, int *refs) {
    printf(CLR_BOLD CLR_MAGENTA
           "\n=====================================================\n"
           " Memory Management Simulator - %s\n"
           "=====================================================\n"
           CLR_RESET, algoName);
    printf(CLR_BOLD "Page size    : " CLR_RESET "%d bytes\n", PAGE_SIZE_BYTES);
    printf(CLR_BOLD "Frame count  : " CLR_RESET "%d\n", NUM_FRAMES);
    printf(CLR_BOLD "Total memory : " CLR_RESET "%d bytes\n", NUM_FRAMES * PAGE_SIZE_BYTES);
    printf(CLR_BOLD "Reference string: " CLR_RESET);
    for (int i = 0; i < NUM_REFERENCES; i++) printf("P%d ", refs[i]);
    printf("\n\n");
}

/* =====================================================================
   FIFO: whichever page has been in memory the LONGEST (smallest
   arrivalTick) gets evicted first, regardless of recent use.
===================================================================== */
static SimResult runFIFO(int *refs) {
    MemoryFrame mem[NUM_FRAMES];
    for (int f = 0; f < NUM_FRAMES; f++) {
        mem[f].pageNumber = -1;
        mem[f].arrivalTick = 0;
        mem[f].touchedTick = 0;
        memset(mem[f].payload, 0, PAGE_SIZE_BYTES);
    }

    SimResult result = {0, 0};
    int tick = 0;

    printBanner("FIFO (First-In First-Out)", refs);

    for (int i = 0; i < NUM_REFERENCES; i++) {
        int page = refs[i];
        tick++;

        int where = locatePage(mem, page);
        if (where != -1) {
            mem[where].touchedTick = tick;
            result.hitCount++;
            showMemoryRow(mem, page, 0);
            continue;
        }

        result.faultCount++;

        int freeSlot = locateFreeFrame(mem);
        if (freeSlot != -1) {
            loadPageIntoFrame(&mem[freeSlot], page, tick);
        } else {
            /* evict the frame that arrived earliest */
            int victim = 0;
            for (int f = 1; f < NUM_FRAMES; f++) {
                if (mem[f].arrivalTick < mem[victim].arrivalTick) victim = f;
            }
            printf(CLR_RED "     evicting P%d (arrived at tick %d)" CLR_RESET "\n",
                   mem[victim].pageNumber, mem[victim].arrivalTick);
            loadPageIntoFrame(&mem[victim], page, tick);
        }
        showMemoryRow(mem, page, 1);
    }
    return result;
}

/* =====================================================================
   LRU: whichever page has gone the LONGEST without being touched
   (smallest touchedTick) gets evicted.
===================================================================== */
static SimResult runLRU(int *refs) {
    MemoryFrame mem[NUM_FRAMES];
    for (int f = 0; f < NUM_FRAMES; f++) {
        mem[f].pageNumber = -1;
        mem[f].arrivalTick = 0;
        mem[f].touchedTick = 0;
        memset(mem[f].payload, 0, PAGE_SIZE_BYTES);
    }

    SimResult result = {0, 0};
    int tick = 0;

    printBanner("LRU (Least Recently Used)", refs);

    for (int i = 0; i < NUM_REFERENCES; i++) {
        int page = refs[i];
        tick++;

        int where = locatePage(mem, page);
        if (where != -1) {
            mem[where].touchedTick = tick;   // refresh recency
            result.hitCount++;
            showMemoryRow(mem, page, 0);
            continue;
        }

        result.faultCount++;

        int freeSlot = locateFreeFrame(mem);
        if (freeSlot != -1) {
            loadPageIntoFrame(&mem[freeSlot], page, tick);
        } else {
            /* evict the frame touched least recently */
            int victim = 0;
            for (int f = 1; f < NUM_FRAMES; f++) {
                if (mem[f].touchedTick < mem[victim].touchedTick) victim = f;
            }
            printf(CLR_YELLOW "     evicting P%d (last touched at tick %d)" CLR_RESET "\n",
                   mem[victim].pageNumber, mem[victim].touchedTick);
            loadPageIntoFrame(&mem[victim], page, tick);
        }
        showMemoryRow(mem, page, 1);
    }
    return result;
}

static void printResultSummary(const char *label, SimResult r) {
    float hitPct = (100.0f * r.hitCount) / NUM_REFERENCES;
    float faultPct = (100.0f * r.faultCount) / NUM_REFERENCES;

    printf(CLR_BOLD "\n%s summary:\n" CLR_RESET, label);
    printf("  References : %d\n", NUM_REFERENCES);
    printf("  Hits       : " CLR_GREEN "%d (%.1f%%)" CLR_RESET "\n", r.hitCount, hitPct);
    printf("  Faults     : " CLR_RED "%d (%.1f%%)" CLR_RESET "\n", r.faultCount, faultPct);
}

static void printComparisonTable(SimResult fifo, SimResult lru) {
    printf(CLR_BOLD CLR_MAGENTA
           "\n=====================================================\n"
           " Comparison: FIFO vs LRU\n"
           "=====================================================\n"
           CLR_RESET);

    printf(CLR_BOLD "%-12s %-8s %-8s %-10s\n" CLR_RESET,
           "Algorithm", "Hits", "Faults", "Hit Rate");
    printf("%-12s " CLR_GREEN "%-8d" CLR_RESET CLR_RED "%-8d" CLR_RESET "%.1f%%\n",
           "FIFO", fifo.hitCount, fifo.faultCount,
           (100.0f * fifo.hitCount) / NUM_REFERENCES);
    printf("%-12s " CLR_GREEN "%-8d" CLR_RESET CLR_RED "%-8d" CLR_RESET "%.1f%%\n",
           "LRU", lru.hitCount, lru.faultCount,
           (100.0f * lru.hitCount) / NUM_REFERENCES);

    printf("\nVerdict: ");
    if (lru.faultCount < fifo.faultCount) {
        printf(CLR_GREEN "LRU wins with fewer faults (%d vs %d)\n" CLR_RESET,
               lru.faultCount, fifo.faultCount);
    } else if (fifo.faultCount < lru.faultCount) {
        printf(CLR_YELLOW "FIFO wins with fewer faults (%d vs %d)\n" CLR_RESET,
               fifo.faultCount, lru.faultCount);
    } else {
        printf(CLR_CYAN "Both algorithms tied on faults.\n" CLR_RESET);
    }
}

int main(void) {
    /* The sequence of pages the CPU requests, in order.
       With only NUM_FRAMES physical frames, some of these will
       cause the earlier pages to be evicted. */
    int referenceString[NUM_REFERENCES] = {2, 4, 1, 4, 2, 5, 1, 2, 4, 5};

    SimResult fifoResult = runFIFO(referenceString);
    printResultSummary("FIFO", fifoResult);

    printf("\n");

    SimResult lruResult = runLRU(referenceString);
    printResultSummary("LRU", lruResult);

    printComparisonTable(fifoResult, lruResult);

    printf("\n");
    return 0;
}
