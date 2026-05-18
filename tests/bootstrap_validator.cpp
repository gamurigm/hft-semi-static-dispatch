#include <iostream>
#include <vector>
#include <chrono>
#include <random>
#include <algorithm>
#include <numeric>
#include <iomanip>
#include <cmath>
#include "../include/semi_static.hpp"

// Define dummy functions for the router
volatile long long dummy_sum = 0;
void fnA(double p, int c) { dummy_sum += p * c * 1; }
void fnB(double p, int c) { dummy_sum += p * c * 2; }
void fnC(double p, int c) { dummy_sum += p * c * 3; }
void fnD(double p, int c) { dummy_sum += p * c * 4; }

struct TrialResult {
    double switch_ns;
    double flex_ns;
    double fast_ns;
    double control_ns;
};

TrialResult run_trial(int seed) {
    const int N = 5'000'000;
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> dist(0, 3);
    std::vector<int> conds(N);
    for(int i=0; i<N; ++i) conds[i] = dist(rng);

    TrialResult res;
    
    // 1. switch
    auto t0 = std::chrono::high_resolution_clock::now();
    for(int i=0; i<N; ++i) {
        switch(conds[i]) {
            case 0: fnA(1.0, 1); break;
            case 1: fnB(1.0, 1); break;
            case 2: fnC(1.0, 1); break;
            case 3: fnD(1.0, 1); break;
        }
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    res.switch_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / N;

    // 2. FlexBranch
    semistatic::FlexBranch<void(double, int)> flex(fnA, fnB, fnC, fnD);
    t0 = std::chrono::high_resolution_clock::now();
    for(int i=0; i<N; ++i) {
        if (i % 1000 == 0) flex.set(conds[i]);
        flex(1.0, 1);
    }
    t1 = std::chrono::high_resolution_clock::now();
    res.flex_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / N;

    // 3. FastBranch
    semistatic::FastBranch<4, void, double, int> fast(fnA, fnB, fnC, fnD);
    t0 = std::chrono::high_resolution_clock::now();
    for(int i=0; i<N; ++i) {
        if (i % 1000 == 0) fast.set(conds[i]);
        fast(1.0, 1);
    }
    t1 = std::chrono::high_resolution_clock::now();
    res.fast_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / N;

    // 4. Control (Direct call)
    t0 = std::chrono::high_resolution_clock::now();
    for(int i=0; i<N; ++i) {
        fnA(1.0, 1); // No dispatch overhead
    }
    t1 = std::chrono::high_resolution_clock::now();
    res.control_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / N;

    return res;
}

void run_bootstrap(const std::vector<double>& a, const std::vector<double>& b, const std::string& name) {
    const int B = 10000;
    int n = a.size();
    std::mt19937 rng(1337);
    std::uniform_int_distribution<int> dist(0, n - 1);
    
    std::vector<double> diffs(B);
    int p_count = 0;
    
    for(int i=0; i<B; ++i) {
        double sum_a = 0, sum_b = 0;
        for(int j=0; j<n; ++j) {
            int idx = dist(rng);
            sum_a += a[idx];
            sum_b += b[idx];
        }
        double mean_a = sum_a / n;
        double mean_b = sum_b / n;
        diffs[i] = mean_a - mean_b;
        if (diffs[i] <= 0) p_count++;
    }
    
    std::sort(diffs.begin(), diffs.end());
    double ci_low = diffs[250];
    double ci_high = diffs[9750];
    double p_val = (double)p_count / B;
    if (p_val == 0.0) p_val = 0.0001; // < 0.0001
    
    // Cohen's d for original sample
    double mean_a=0, mean_b=0, var_a=0, var_b=0;
    for(int i=0; i<n; ++i) { mean_a += a[i]; mean_b += b[i]; }
    mean_a /= n; mean_b /= n;
    for(int i=0; i<n; ++i) { var_a += std::pow(a[i]-mean_a, 2); var_b += std::pow(b[i]-mean_b, 2); }
    var_a /= (n-1); var_b /= (n-1);
    double pooled_sd = std::sqrt((var_a + var_b)/2.0);
    double d = (mean_a - mean_b) / pooled_sd;

    std::cout << std::left << std::setw(30) << name 
              << " | CI: [" << std::fixed << std::setprecision(2) << ci_low << ", " << ci_high << "] ns "
              << "| d = " << std::setprecision(2) << d << " "
              << "| p = " << std::setprecision(4) << p_val << "\n";
}

int main() {
    const int K = 100;
    std::cout << "Running " << K << " Monte Carlo trials (5M ops each)...\n";
    
    std::vector<double> v_switch(K), v_flex(K), v_fast(K), v_control(K);
    
    for(int i=0; i<K; ++i) {
        TrialResult r = run_trial(42 + i);
        v_switch[i] = r.switch_ns;
        v_flex[i] = r.flex_ns;
        v_fast[i] = r.fast_ns;
        v_control[i] = r.control_ns;
        if ((i+1) % 25 == 0) std::cout << "Completed " << i+1 << " trials...\n";
    }
    
    std::cout << "\nBootstrapping Analysis (B = 10,000 resamples)...\n";
    std::cout << std::string(80, '-') << "\n";
    run_bootstrap(v_switch, v_fast, "switch vs FastBranch");
    run_bootstrap(v_switch, v_flex, "switch vs FlexBranch");
    run_bootstrap(v_flex, v_fast, "FlexBranch vs FastBranch");
    run_bootstrap(v_fast, v_control, "FastBranch vs Control");
    
    // Calculate Speedup CI
    const int B = 10000;
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, K - 1);
    std::vector<double> speedups(B);
    for(int i=0; i<B; ++i) {
        double sum_sw = 0, sum_fa = 0;
        for(int j=0; j<K; ++j) {
            int idx = dist(rng);
            sum_sw += v_switch[idx];
            sum_fa += v_fast[idx];
        }
        speedups[i] = (sum_sw/K) / (sum_fa/K);
    }
    std::sort(speedups.begin(), speedups.end());
    std::cout << "\nSpeedup FastBranch (Empirical CI 95%): [" 
              << std::fixed << std::setprecision(2) << speedups[250] << "x, " << speedups[9750] << "x]\n";
              
    return 0;
}
