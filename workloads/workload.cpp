#include <iostream>
#include <vector>
#include <random>
#include <chrono>
#include <unordered_map>
#include <cmath>

using namespace std;

/* ------------------- DATA GENERATION ------------------- */

vector<double> generate_data(size_t N) {
    vector<double> data(N);

    mt19937 rng(123);
    uniform_real_distribution<double> dist(0.0, 1.0);

    for (size_t i = 0; i < N; i++)
        data[i] = dist(rng);

    return data;
}

/* ------------------- GRAPH STRUCTURE ------------------- */

vector<vector<int>> build_graph(int nodes, int degree) {

    mt19937 rng(42);
    uniform_int_distribution<int> dist(0, nodes-1);

    vector<vector<int>> graph(nodes);

    for (int i = 0; i < nodes; i++) {

        for (int j = 0; j < degree; j++) {
            graph[i].push_back(dist(rng));
        }
    }

    return graph;
}

double graph_score(const vector<vector<int>>& graph,
                   const vector<double>& weights)
{
    double score = 0;

    for (size_t i = 0; i < graph.size(); i++) {

        for (int nbr : graph[i]) {

            score += weights[nbr] * 0.000001;
        }
    }

    return score;
}

/* ------------------- MATRIX OPERATIONS ------------------- */

void matrix_transform(vector<double>& M,
                      const vector<double>& data,
                      int N)
{
    for (int i = 0; i < N; i++) {

        for (int j = 0; j < N; j++) {

            double v = M[i*N + j];

            v += data[(i+j) % data.size()];
            v = sin(v);

            M[i*N + j] = v;
        }
    }
}

/* ------------------- HISTOGRAM ------------------- */

vector<int> compute_histogram(const vector<double>& data,
                              int bins)
{
    vector<int> hist(bins, 0);

    for (double x : data) {

        int b = (int)(x * bins) % bins;

        hist[b]++;
    }

    return hist;
}

/* ------------------- ITERATIVE UPDATE ------------------- */

void iterative_update(vector<double>& data,
                      const vector<int>& hist)
{
    for (size_t i = 0; i < data.size(); i++) {

        data[i] += hist[i % hist.size()] * 1e-6;

        data[i] = sqrt(data[i] + 1.0);
    }
}

/* ------------------- MAIN DRIVER ------------------- */

int main() {

    const int MATRIX_N = 2500;     // ~50MB matrix
    const size_t DATA_SIZE = 2'000'000;  // ~16MB
    const int GRAPH_NODES = 200000;
    const int DEGREE = 6;

    cout << "Allocating memory..." << endl;

    auto data = generate_data(DATA_SIZE);

    vector<double> matrix(MATRIX_N * MATRIX_N, 0.5);

    auto graph = build_graph(GRAPH_NODES, DEGREE);

    cout << "Running workload..." << endl;

    auto start = chrono::high_resolution_clock::now();

    int iterations = 0;

    while (true) {

        matrix_transform(matrix, data, MATRIX_N);

        auto hist = compute_histogram(data, 512);

        iterative_update(data, hist);

        double s = graph_score(graph, data);

        if (iterations % 2 == 0)
            cout << "score = " << s << endl;

        iterations++;

        auto now = chrono::high_resolution_clock::now();
        chrono::duration<double> elapsed = now - start;

        if (elapsed.count() > 12)
            break;
    }

    cout << "iterations = " << iterations << endl;

    return 0;
}
