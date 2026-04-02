#include <iostream>
#include <vector>
#include <chrono>
#include <random>

using namespace std;

int main() {
    const int N = 1600;  // adjust for memory size
    const int REPEAT_SECONDS = 10;

    cout << "Allocating matrices..." << endl;

    // allocate matrices (contiguous memory)
    vector<double> A(N * N);
    vector<double> B(N * N);
    vector<double> C(N * N, 0.0);

    // initialize matrices with deterministic pseudo-random values
    mt19937 rng(42);
    uniform_real_distribution<double> dist(0.0, 1.0);

    for (int i = 0; i < N*N; i++) {
        A[i] = dist(rng);
        B[i] = dist(rng);
    }

    cout << "Running matrix multiplication for ~10 seconds..." << endl;

    auto start = chrono::high_resolution_clock::now();

    int iterations = 0;

    while (true) {
        for (int i = 0; i < N; i++) {
            for (int k = 0; k < N; k++) {
                double a = A[i*N + k];

                for (int j = 0; j < N; j++) {
                    C[i*N + j] += a * B[k*N + j];
                }
            }
        }

        iterations++;

        auto now = chrono::high_resolution_clock::now();
        chrono::duration<double> elapsed = now - start;

        if (elapsed.count() > REPEAT_SECONDS)
            break;
    }

    cout << "Iterations completed: " << iterations << endl;

    // prevent compiler from optimizing away computation
    double checksum = 0;
    for (int i = 0; i < N*N; i++)
        checksum += C[i];

    cout << "Checksum: " << checksum << endl;

    return 0;
}