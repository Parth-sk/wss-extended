#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>

using namespace std;

// -------- Configuration --------
#define PHASE_A_SIZE (1ULL << 20)   // 1 MB - tight loop
#define PHASE_B_SIZE (10ULL << 20)  // 10 MB - sequential scan
#define PHASE_C_SIZE (9ULL << 20)   // 9 MB - random access
#define PHASE_DURATION_SEC 10       // 10 seconds per phase
#define NUM_PHASES 3

inline void mark(const char* label) {
    asm volatile("" ::: "memory");
    (void)label;
}

// -------- Phase A: Tight loop over small region --------
void phase_a(char* buffer, size_t size) {
    mark("PHASE_A_ENTER");
    cout << "Phase A: Tight loop over " << (size / (1 << 20)) << " MB region"
         << endl;

    auto start = chrono::steady_clock::now();
    auto end = start + chrono::seconds(PHASE_DURATION_SEC);

    volatile char sum = 0;
    size_t idx = 0;

    while (chrono::steady_clock::now() < end) {
        // Tight loop with high cache locality
        for (size_t i = 0; i < size; i += 64) {
            sum += buffer[i];
        }
        idx++;
    }

    cout << "  Completed " << idx << " iterations" << endl;
    mark("PHASE_A_EXIT");
}

// -------- Phase B: Sequential scan of large region --------
void phase_b(char* buffer, size_t size) {
    mark("PHASE_B_ENTER");
    cout << "Phase B: Sequential scan of " << (size / (1 << 20)) << " MB region"
         << endl;

    auto start = chrono::steady_clock::now();
    auto end = start + chrono::seconds(PHASE_DURATION_SEC);

    volatile char sum = 0;
    size_t scans = 0;

    while (chrono::steady_clock::now() < end) {
        // Sequential scan through entire region (streaming)
        for (size_t i = 0; i < size; i += 4096) {
            sum += buffer[i];
        }
        scans++;
    }

    cout << "  Completed " << scans << " full scans" << endl;
    mark("PHASE_B_EXIT");
}

// -------- Phase C: Random access over larger region --------
void phase_c(char* buffer, size_t size, mt19937_64& rng) {
    mark("PHASE_C_ENTER");
    cout << "Phase C: Random access over " << (size / (1 << 20)) << " MB region"
         << endl;

    auto start = chrono::steady_clock::now();
    auto end = start + chrono::seconds(PHASE_DURATION_SEC);

    uniform_int_distribution<size_t> offset_dist(0, size - 1);
    volatile char sum = 0;
    size_t accesses = 0;

    while (chrono::steady_clock::now() < end) {
        // Random access pattern (high page fault rate)
        for (size_t i = 0; i < 10000; i++) {
            size_t offset =
                offset_dist(rng) & ~(4095ULL);  // Align to page boundary
            sum += buffer[offset];
            accesses++;
        }
    }

    cout << "  Completed " << accesses << " random accesses" << endl;
    mark("PHASE_C_EXIT");
}

// -------- Main --------
int main() {
    cout << "Process ID: " << getpid() << endl;
    cout << "Starting phase-changing memory access workload..." << endl;
    cout << endl;

    mt19937_64 rng(
        chrono::high_resolution_clock::now().time_since_epoch().count());

    // Allocate buffers for each phase
    cout << "Allocating memory..." << endl;
    char* buffer_a = new char[PHASE_A_SIZE];
    char* buffer_b = new char[PHASE_B_SIZE];
    char* buffer_c = new char[PHASE_C_SIZE];

    // Initialize buffers to fault pages in
    cout << "Initializing buffers..." << endl;
    memset(buffer_a, 0xAA, PHASE_A_SIZE);
    memset(buffer_b, 0xBB, PHASE_B_SIZE);
    memset(buffer_c, 0xCC, PHASE_C_SIZE);

    cout << endl << "=== Starting Phases ===" << endl << endl;

    // Run phases
    phase_a(buffer_a, PHASE_A_SIZE);
    cout << endl;

    phase_b(buffer_b, PHASE_B_SIZE);
    cout << endl;

    phase_c(buffer_c, PHASE_C_SIZE, rng);
    cout << endl;

    // Cleanup
    cout << "=== Cleanup ===" << endl;
    delete[] buffer_a;
    delete[] buffer_b;
    delete[] buffer_c;

    cout << "Completed." << endl;
    return 0;
}
