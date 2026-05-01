#include <unistd.h>

#include <chrono>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

using namespace std;

// -------- Configuration --------
#define MAX_BLOCKS 100000
#define MIN_SIZE 64
#define MAX_SIZE (1 << 12)
#define DURATION_SEC 30
#define MAX_MEMORY (20ULL << 20)  // 20 MB

struct Block {
    size_t size;
    char* ptr;
};

inline void mark(const char* label) {
    asm volatile("" ::: "memory");
    (void)label;
}

// -------- Allocation Phase --------
void allocate_block(vector<Block>& live_blocks, mt19937_64& rng,
                    size_t min_size, size_t max_size, size_t max_blocks,
                    size_t& total_allocated, size_t max_memory) {
    mark("ALLOC_ENTER");

    if (live_blocks.size() >= max_blocks) return;

    uniform_int_distribution<size_t> size_dist(min_size, max_size);
    size_t sz = size_dist(rng);

    // Check if allocation would exceed memory limit
    if (total_allocated + sz > max_memory) {
        mark("ALLOC_EXIT");
        return;
    }

    char* mem = new char[sz];

    mark("ALLOC_TOUCH");
    for (size_t j = 0; j < sz; j += 4096) {
        mem[j] = (char)(j % 256);
    }

    live_blocks.push_back({sz, mem});
    total_allocated += sz;

    mark("ALLOC_EXIT");
}

// -------- Free Phase --------
void free_block(vector<Block>& live_blocks, mt19937_64& rng,
                size_t& total_allocated) {
    mark("FREE_ENTER");

    if (live_blocks.empty()) return;

    size_t idx = rng() % live_blocks.size();

    total_allocated -= live_blocks[idx].size;
    delete[] live_blocks[idx].ptr;

    live_blocks[idx] = live_blocks.back();
    live_blocks.pop_back();

    mark("FREE_EXIT");
}

// -------- Mutation Phase --------
void mutate_block(vector<Block>& live_blocks, mt19937_64& rng) {
    mark("MUTATE_ENTER");

    if (live_blocks.empty()) return;

    size_t idx = rng() % live_blocks.size();
    Block& b = live_blocks[idx];

    mark("MUTATE_TOUCH");

    size_t stride = 4096 * (1 + rng() % 16);
    for (size_t j = 0; j < b.size; j += stride) {
        b.ptr[j] ^= 1;
    }

    mark("MUTATE_EXIT");
}

// -------- Cleanup Phase --------
void cleanup_half(vector<Block>& live_blocks, size_t& total_allocated) {
    mark("CLEANUP_ENTER");

    size_t to_free = live_blocks.size() / 2;

    for (size_t i = 0; i < to_free; i++) {
        total_allocated -= live_blocks.back().size;
        delete[] live_blocks.back().ptr;
        live_blocks.pop_back();
    }

    mark("CLEANUP_EXIT");
}

// -------- Main --------
int main() {
    cout << "Process ID: " << getpid() << endl;
    cout << "Starting allocator churn workload..." << endl;
    cout << "Running for " << DURATION_SEC << " seconds" << endl;

    vector<Block> live_blocks;
    live_blocks.reserve(MAX_BLOCKS);

    size_t total_allocated = 0;

    mt19937_64 rng(
        chrono::high_resolution_clock::now().time_since_epoch().count());
    uniform_int_distribution<int> action_dist(0, 99);

    auto start = chrono::steady_clock::now();
    auto end = start + chrono::seconds(DURATION_SEC);

    size_t ops = 0;

    while (chrono::steady_clock::now() < end) {
        int action = action_dist(rng);

        if (action < 40) {
            allocate_block(live_blocks, rng, MIN_SIZE, MAX_SIZE, MAX_BLOCKS,
                           total_allocated, MAX_MEMORY);
        } else if (action < 80) {
            free_block(live_blocks, rng, total_allocated);
        } else {
            mutate_block(live_blocks, rng);
        }

        ops++;
    }

    // Cleanup remaining
    for (auto& b : live_blocks) {
        total_allocated -= b.size;
        delete[] b.ptr;
    }

    return 0;
}