/*
 * wss-v1.c	Estimate the working set size (WSS) for a process on Linux.
 *		Version 1: suited for small processes.
 *
 * This is a proof of concept that uses idle page tracking from Linux 4.3+,
 * for a page-based WSS estimation. This version walks page structures one by
 * one, and is suited for small processes. See wss-v2.c which snapshots page
 * data, and can be over 50x faster for large processes, although slower for
 * small processes. There is also wss.pl, which can be over 500x faster and
 * works on older Linux, however, uses the referenced page flag instead and has
 * its own caveats. These tools can be found here:
 *
 * http://www.brendangregg.com/wss.pl
 *
 * Currently written for x86_64 and default page size only. Early version:
 * probably has bugs.
 *
 * COMPILE: gcc -o wss-v1 wss-v1.c
 *
 * REQUIREMENTS: Linux 4.3+
 *
 * USAGE: wss PID duration(s) intervals
 *
 * COLUMNS:
 *	- Interval: Measurement interval number (0-based).
 *	- Ref(MB): Referenced (Mbytes) during the interval duration.
 *	           This is the working set size metric.
 *	- Pages:   Number of unique pages accessed in the interval.
 *
 * WARNING: This tool sets and reads process page flags, which for large
 * processes (> 100 Gbytes) can take several minutes (use wss-v2 for those
 * instead). During that time, this tool consumes one CPU, and the application
 * may experience slightly higher latency (eg, 5%). Consider these overheads.
 * Also, this is activating some new kernel code added in Linux 4.3 that you
 * may have never executed before. As is the case for any such code, there is
 * the risk of undiscovered kernel panics (I have no specific reason to worry,
 * just being paranoid). Test in a lab environment for your kernel versions,
 * and consider this experimental: use at your own risk.
 *
 * Copyright 2018 Netflix, Inc.
 * Licensed under the Apache License, Version 2.0 (the "License")
 *
 * 13-Jan-2018	Brendan Gregg	Created this.
 *
 */
#define _POSIX_C_SOURCE 199309L

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/user.h>
#include <time.h>
#include <unistd.h>

// see Documentation/vm/pagemap.txt:
#define PFN_MASK (~(0x1ffLLU << 55))

#define PATHSIZE 128
#define LINESIZE 256
#define PAGEMAP_CHUNK_SIZE 8
#define MAX_PAGES_PER_INTERVAL 100000
#define MAX_INTERVALS 100

// from mm/page_idle.c:
#ifndef BITMAP_CHUNK_SIZE
#define BITMAP_CHUNK_SIZE 8
#endif

#ifndef PAGE_OFFSET
#define PAGE_OFFSET 0xffff880000000000LLU
#endif

enum { READIDLE = 0, SETIDLE };

// interval measurement structure
typedef struct {
    double start;  // timestamp before SETIDLE
    double end;    // timestamp after READIDLE
    unsigned long long pages[MAX_PAGES_PER_INTERVAL];  // array of accessed PFNs
    int page_count;  // number of pages accessed
} IntervalMeasurement;

// globals
int g_debug = 0;  // 1 == some, 2 == verbose

int mapidle(pid_t pid, unsigned long long mapstart, unsigned long long mapend,
            int action, IntervalMeasurement* measurement) {
    char pagepath[PATHSIZE];
    char* idlepath = "/sys/kernel/mm/page_idle/bitmap";
    int idlefd, pagefd;
    int pagesize = getpagesize();
    unsigned long long p, pagemap_offset, pfn, idlemapp, idlebits;
    int flags;
    int err = 0;

    // open pagemap for virtual to PFN translation
    if (sprintf(pagepath, "/proc/%d/pagemap", pid) < 0) {
        printf("Can't allocate memory. Exiting.");
        exit(1);
    }
    if ((pagefd = open(pagepath, O_RDONLY)) < 0) {
        perror("Can't read pagemap file");
        return 2;
    }

    // open idlemap for WSS estimation
    flags = O_RDONLY;
    if (action == SETIDLE) flags = O_WRONLY;
    if ((idlefd = open(idlepath, flags)) < 0) {
        perror("Can't read idlemap file");
        close(pagefd);
        return 2;
    }

    // walk pagemap to get PFN, then operate on PFN from idlemap
    for (p = mapstart; p < mapend; p += pagesize) {
        pagemap_offset = PAGEMAP_CHUNK_SIZE * p / pagesize;

        /*
         * The following involves a lot of syscalls for setting and
         * reading bits. This is why this program is slow. This should
         * be optimized to read by chunks. Or to use mmap, however, I
         * don't think the kernel files involved support an mmap
         * interface. Perhaps a later version of Linux will provide a
         * /proc/PID/clear_idle interface with an entry in
         * /proc/PID/smaps, which would make this much faster.
         */

        // convert virtual address p to physical PFN
        if (lseek(pagefd, pagemap_offset, SEEK_SET) < 0) {
            printf("Can't seek pagemap file\n");
            goto out;
        }
        // reading entire pagemap entry (8 bytes) for the page, which includes
        // the PFN and flags
        if (read(pagefd, &pfn, sizeof(pfn)) < 0) {
            printf("Can't read pagemap file\n");
            goto out;
        }
        // mask to get PFN only (55 bits from right)
        pfn = pfn & PFN_MASK;
        if (pfn == 0) continue;

        // locate idle map byte
        idlemapp = (pfn / 64) * BITMAP_CHUNK_SIZE;
        if (lseek(idlefd, idlemapp, SEEK_SET) < 0) {
            printf("Can't seek idlemap file\n");
            goto out;
        }
        if (g_debug > 1) {
            printf("%s: p %llx pfn %llx idlebits %llx\n",
                   action == READIDLE ? "R" : "W", p, pfn, idlebits);
        }

        /*
         * At the time of writing, I could find no example code that
         * used idle page tracking. This is based on the description in
         * Documentation/vm/idle_page_tracking.txt.
         */

        // read idle bit
        if (action == READIDLE) {
            if (read(idlefd, &idlebits, sizeof(idlebits)) <= 0) {
                perror("Can't read idlemap file");
                goto out;
            }
            if (!(idlebits & (1ULL << (pfn % 64)))) {
                // store PFN if within bounds
                if (measurement->page_count < MAX_PAGES_PER_INTERVAL) {
                    measurement->pages[measurement->page_count] = p / pagesize; //store the virtual page number, since perf output is in virtual addresses
                    measurement->page_count++;
                }
            }

            // set idle bit
        } else /* SETIDLE */ {
            idlebits = ~0ULL;
            if (write(idlefd, &idlebits, sizeof(idlebits)) <= 0) {
                perror("Can't write idlemap file");
                goto out;
            }
        }
    }

out:
    close(pagefd);
    close(idlefd);

    return err;
}

int walkmaps(pid_t pid, int action, IntervalMeasurement* measurement) {
    FILE* mapsfile;
    char mapspath[PATHSIZE];
    char line[LINESIZE];
    unsigned long long mapstart, mapend;

    // read virtual mappings
    if (sprintf(mapspath, "/proc/%d/maps", pid) < 0) {
        printf("Can't allocate memory. Exiting.");
        exit(1);
    }
    if ((mapsfile = fopen(mapspath, "r")) == NULL) {
        perror("Can't read maps file");
        exit(2);
    }

    while (fgets(line, sizeof(line), mapsfile) != NULL) {
        sscanf(line, "%llx-%llx", &mapstart, &mapend);
        if (g_debug) printf("MAP %llx-%llx\n", mapstart, mapend);
        if (mapstart > PAGE_OFFSET)
            continue;  // page idle tracking is user mem only
        if (mapidle(pid, mapstart, mapend, action, measurement)) {
            printf("Error setting map %llx-%llx. Exiting.\n", mapstart, mapend);
        }
    }

    fclose(mapsfile);

    return 0;
}

void get_time(double* time) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    *time = ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char* argv[]) {
    pid_t pid;
    double duration, mbytes;
    int intervals, i;

    // options
    if (argc < 4) {
        printf("USAGE: wss PID duration(s) intervals\n");
        exit(0);
    }
    pid = atoi(argv[1]);
    duration = atof(argv[2]);
    intervals = atoi(argv[3]);

    if (duration < 0.01) {
        printf("Interval too short. Exiting.\n");
        return 1;
    }
    if (intervals < 1) {
        printf("Intervals must be >= 1. Exiting.\n");
        return 1;
    }
    if (intervals > MAX_INTERVALS) {
        printf("Intervals exceeds maximum (%d). Exiting.\n", MAX_INTERVALS);
        return 1;
    }

    int pagesize = getpagesize();
    IntervalMeasurement measurements[intervals];

    printf(
        "Watching PID %d page references during %.2f seconds x %d "
        "intervals...\n",
        pid, duration, intervals);

    // run measurement intervals
    for (i = 0; i < intervals; i++) {
        measurements[i].page_count = 0;

        // capture start time and set idle flags
        get_time(&measurements[i].start);
        walkmaps(pid, SETIDLE, &measurements[i]);

        // sleep for duration
        usleep((int)(duration * 1000000));

        // read idle flags and capture end time
        walkmaps(pid, READIDLE, &measurements[i]);
        get_time(&measurements[i].end);
    }

    // output results
    printf("%-8s %12s %12s %10s %10s\n", "Interval", "Start(s)", "End(s)",
           "Ref(MB)", "Pages");
    for (i = 0; i < intervals; i++) {
        mbytes = (measurements[i].page_count * pagesize) / (1024.0 * 1024.0);
        printf("%-8d %12.6f %12.6f %10.2f %10d\n", i, measurements[i].start,
               measurements[i].end, mbytes, measurements[i].page_count);
    }

    return 0;
}
