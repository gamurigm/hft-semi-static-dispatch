/**
 * ============================================================
 *  ANALISIS MONTE CARLO — Validacion Estadistica
 *  de Semi-Static Conditions (FastBranch vs switch)
 *
 *  Metodologia:
 *    - K = 30 trials independientes (semillas aleatorias distintas)
 *    - N = 5,000,000 operaciones por trial
 *    - Se mide latencia total de cada enfoque en cada trial
 *    - Se calcula: media, desviacion estandar, IC 95%, speedup
 *    - Test de hipotesis t-Student para validar significancia
 *    - Histograma ASCII de la distribucion de tiempos
 *
 *  Compilar:
 *    g++ -std=c++17 -O3 -o montecarlo.exe montecarlo_analysis.cpp
 *    .\montecarlo.exe
 * ============================================================
 */

#include "semi_static.hpp"

#include <iostream>
#include <iomanip>
#include <vector>
#include <random>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <string>

using namespace semistatic;
using Clock = std::chrono::high_resolution_clock;

// ============================================================
//  CONFIGURACION
// ============================================================

const int K_TRIALS   = 100;       // numero de replicas Monte Carlo
const int N_OPS      = 5'000'000; // operaciones por trial
const int N_RAMAS    = 4;         // numero de ramas (switch de 4 casos)
const int SET_FREQ   = 1000;      // FastBranch: set_direction cada N ops

// ============================================================
//  FUNCIONES RAMA (trabajo controlado)
// ============================================================

volatile double SINK = 0.0;

void fn_bid (double p, int q) { SINK += p * q * 1.0002; }
void fn_ask (double p, int q) { SINK += p * q * 0.9998; }
void fn_mid (double p, int q) { SINK += p * q * 1.0000; }
void fn_vwap(double p, int q) { SINK += p * q / (q + 1); }

// ============================================================
//  ESTADISTICAS
// ============================================================

struct Stats {
    double media;
    double desv_std;
    double ic95_low;
    double ic95_high;
    double mediana;
    double min_val;
    double max_val;
};

Stats calcular_stats(std::vector<double>& datos) {
    int n = (int)datos.size();
    std::sort(datos.begin(), datos.end());

    double sum  = std::accumulate(datos.begin(), datos.end(), 0.0);
    double mean = sum / n;

    double sq_sum = 0;
    for (double d : datos) sq_sum += (d - mean) * (d - mean);
    double sd = std::sqrt(sq_sum / (n - 1));  // desviacion estandar muestral

    // t-Student para IC 95% con n-1 grados de libertad
    // Para n=30, t_0.025 ≈ 2.045
    double t_crit = 2.045;
    double margin = t_crit * sd / std::sqrt(n);

    Stats s;
    s.media     = mean;
    s.desv_std  = sd;
    s.ic95_low  = mean - margin;
    s.ic95_high = mean + margin;
    s.mediana   = (n % 2 == 0) ? (datos[n/2-1] + datos[n/2]) / 2.0 : datos[n/2];
    s.min_val   = datos.front();
    s.max_val   = datos.back();
    return s;
}

// Test t de Student para muestras independientes (dos colas)
struct TTestResult {
    double t_stat;
    double df;          // grados de libertad (Welch's approximation)
    bool   significativo; // |t| > t_critico para alpha=0.05
    double cohen_d;     // tamano del efecto
};

TTestResult t_test(const Stats& a, const Stats& b, int n) {
    double se = std::sqrt((a.desv_std*a.desv_std + b.desv_std*b.desv_std) / n);
    double t  = (a.media - b.media) / se;

    // Welch's df approximation
    double s1_sq = a.desv_std * a.desv_std;
    double s2_sq = b.desv_std * b.desv_std;
    double num   = (s1_sq/n + s2_sq/n) * (s1_sq/n + s2_sq/n);
    double den   = (s1_sq/n)*(s1_sq/n)/(n-1) + (s2_sq/n)*(s2_sq/n)/(n-1);
    double df    = num / den;

    // t critico para alpha=0.05 bilateral con df~58 ≈ 2.00
    double t_crit = 2.00;
    bool sig = std::abs(t) > t_crit;

    // Cohen's d (tamano del efecto)
    double pooled_sd = std::sqrt((s1_sq + s2_sq) / 2.0);
    double d = std::abs(a.media - b.media) / pooled_sd;

    return { t, df, sig, d };
}

// ============================================================
//  HISTOGRAMA ASCII
// ============================================================

void histograma(const std::vector<double>& datos, const char* nombre, int bins = 15) {
    double lo = *std::min_element(datos.begin(), datos.end());
    double hi = *std::max_element(datos.begin(), datos.end());
    double rng = hi - lo;
    if (rng < 0.001) rng = 1.0;
    double bin_w = rng / bins;

    std::vector<int> counts(bins, 0);
    for (double d : datos) {
        int b = std::min((int)((d - lo) / bin_w), bins - 1);
        counts[b]++;
    }
    int max_c = *std::max_element(counts.begin(), counts.end());

    std::cout << "  " << nombre << " (distribucion de " << datos.size() << " trials):\n";
    for (int i = 0; i < bins; i++) {
        double edge = lo + i * bin_w;
        int bar = (max_c > 0) ? counts[i] * 40 / max_c : 0;
        std::cout << "  " << std::fixed << std::setprecision(1) << std::setw(6) << edge
                  << " ms |" << std::string(bar, '#')
                  << " (" << counts[i] << ")\n";
    }
    std::cout << "\n";
}

// ============================================================
//  EJECUCION DE UN TRIAL
// ============================================================

struct TrialResult {
    double ms_switch;
    double ms_fast;
    double ms_flex;
    double ms_directo;
};

TrialResult ejecutar_trial(unsigned int seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> dist(0, N_RAMAS - 1);

    // Pre-generar condiciones aleatorias para este trial
    std::vector<int> conds(N_OPS);
    for (auto& c : conds) c = dist(rng);

    TrialResult r;

    // A) switch aleatorio
    SINK = 0;
    auto t0 = Clock::now();
    for (int i = 0; i < N_OPS; i++) {
        double p = 150.0 + (i%10); int q = 100 + (i%50);
        switch (conds[i]) {
            case 0: fn_bid(p,q); break; case 1: fn_ask(p,q); break;
            case 2: fn_mid(p,q); break; case 3: fn_vwap(p,q); break;
        }
    }
    r.ms_switch = std::chrono::duration<double,std::milli>(Clock::now()-t0).count();

    // B) FastBranch (set/1000)
    SINK = 0;
    auto fb = make_fast(fn_bid, fn_ask, fn_mid, fn_vwap);
    t0 = Clock::now();
    for (int i = 0; i < N_OPS; i++) {
        if (i % SET_FREQ == 0) fb.set(conds[i]);
        fb(150.0 + (i%10), 100 + (i%50));
    }
    r.ms_fast = std::chrono::duration<double,std::milli>(Clock::now()-t0).count();

    // C) FlexBranch (set/1000)
    SINK = 0;
    auto fl = make_flex<void(double,int)>(fn_bid, fn_ask, fn_mid, fn_vwap);
    t0 = Clock::now();
    for (int i = 0; i < N_OPS; i++) {
        if (i % SET_FREQ == 0) fl.set(conds[i]);
        fl(150.0 + (i%10), 100 + (i%50));
    }
    r.ms_flex = std::chrono::duration<double,std::milli>(Clock::now()-t0).count();

    // D) Directo (0 branches, caso ideal)
    SINK = 0;
    t0 = Clock::now();
    for (int i = 0; i < N_OPS; i++)
        fn_bid(150.0 + (i%10), 100 + (i%50));
    r.ms_directo = std::chrono::duration<double,std::milli>(Clock::now()-t0).count();

    return r;
}


// ============================================================
//  MAIN
// ============================================================

int main() {
    std::cout << "\n";
    std::cout << "  ================================================================\n";
    std::cout << "  ANALISIS MONTE CARLO — Validacion Estadistica\n";
    std::cout << "  Semi-Static Conditions (FastBranch vs switch)\n";
    std::cout << "  ================================================================\n\n";

    std::cout << "  Configuracion:\n";
    std::cout << "    Trials (K):         " << K_TRIALS << "\n";
    std::cout << "    Operaciones/trial:  " << N_OPS/1'000'000 << "M\n";
    std::cout << "    Ramas:              " << N_RAMAS << " (condicion uniforme aleatoria)\n";
    std::cout << "    set_direction freq: 1/" << SET_FREQ << " ops\n";
    std::cout << "    Semillas:           aleatorias independientes\n\n";

    // Generar semillas aleatorias para cada trial
    std::random_device rd;
    std::vector<unsigned int> seeds(K_TRIALS);
    for (auto& s : seeds) s = rd();

    // Ejecutar trials
    std::vector<double> v_switch, v_fast, v_flex, v_directo;
    std::vector<double> v_speedup;

    std::cout << "  Ejecutando " << K_TRIALS << " trials...\n\n";
    std::cout << "  Trial | switch   | FastBranch | FlexBranch | Directo  | Speedup\n";
    std::cout << "  ------+----------+------------+------------+----------+--------\n";

    for (int k = 0; k < K_TRIALS; k++) {
        auto r = ejecutar_trial(seeds[k]);

        v_switch.push_back(r.ms_switch);
        v_fast.push_back(r.ms_fast);
        v_flex.push_back(r.ms_flex);
        v_directo.push_back(r.ms_directo);
        v_speedup.push_back(r.ms_switch / r.ms_fast);

        std::cout << "  " << std::setw(5) << (k+1) << " | "
                  << std::fixed << std::setprecision(1)
                  << std::setw(6) << r.ms_switch << " ms | "
                  << std::setw(8) << r.ms_fast << " ms | "
                  << std::setw(8) << r.ms_flex << " ms | "
                  << std::setw(6) << r.ms_directo << " ms | "
                  << std::setprecision(2) << std::setw(5) << (r.ms_switch/r.ms_fast) << "x\n";
    }

    // Calcular estadisticas
    auto st_sw   = calcular_stats(v_switch);
    auto st_fast = calcular_stats(v_fast);
    auto st_flex = calcular_stats(v_flex);
    auto st_dir  = calcular_stats(v_directo);
    auto st_sp   = calcular_stats(v_speedup);

    std::cout << "\n\n  ESTADISTICAS DESCRIPTIVAS (K=" << K_TRIALS << " trials)\n";
    std::cout << "  ================================================================\n\n";

    auto print_stats = [](const char* name, const Stats& s) {
        std::cout << "  " << std::left << std::setw(14) << name
                  << std::right << std::fixed
                  << " | Media: " << std::setw(6) << std::setprecision(2) << s.media << " ms"
                  << " | SD: " << std::setw(5) << std::setprecision(2) << s.desv_std << " ms"
                  << " | IC95%: [" << std::setprecision(2) << s.ic95_low
                  << ", " << s.ic95_high << "]"
                  << " | Med: " << std::setprecision(2) << s.mediana << "\n";
    };

    print_stats("switch",     st_sw);
    print_stats("FastBranch", st_fast);
    print_stats("FlexBranch", st_flex);
    print_stats("Directo",    st_dir);
    std::cout << "\n";
    print_stats("Speedup F/S", st_sp);

    // Latencia por operacion
    std::cout << "\n\n  LATENCIA POR OPERACION\n";
    std::cout << "  ================================================================\n\n";

    auto print_latency = [](const char* name, const Stats& s) {
        double ns_mean = s.media * 1e6 / N_OPS;
        double ns_low  = s.ic95_low * 1e6 / N_OPS;
        double ns_high = s.ic95_high * 1e6 / N_OPS;
        std::cout << "  " << std::left << std::setw(14) << name
                  << std::right << std::fixed << std::setprecision(2)
                  << " | " << std::setw(5) << ns_mean << " ns/op"
                  << " | IC95%: [" << ns_low << ", " << ns_high << "] ns/op\n";
    };

    print_latency("switch",     st_sw);
    print_latency("FastBranch", st_fast);
    print_latency("FlexBranch", st_flex);
    print_latency("Directo",    st_dir);

    // Test de hipotesis
    std::cout << "\n\n  TEST DE HIPOTESIS (t-Student, alpha=0.05, bilateral)\n";
    std::cout << "  ================================================================\n\n";
    std::cout << "  H0: No hay diferencia significativa entre switch y FastBranch\n";
    std::cout << "  H1: FastBranch es significativamente mas rapido que switch\n\n";

    auto tt1 = t_test(st_sw, st_fast, K_TRIALS);
    std::cout << "  switch vs FastBranch:\n";
    std::cout << "    t-stat     = " << std::setprecision(3) << tt1.t_stat << "\n";
    std::cout << "    df (Welch) = " << std::setprecision(1) << tt1.df << "\n";
    std::cout << "    Cohen's d  = " << std::setprecision(3) << tt1.cohen_d << "\n";
    std::cout << "    Resultado  = " << (tt1.significativo ? "SIGNIFICATIVO (p < 0.05)" : "NO significativo") << "\n";
    if (tt1.cohen_d > 0.8)       std::cout << "    Efecto     = GRANDE (d > 0.8)\n";
    else if (tt1.cohen_d > 0.5)  std::cout << "    Efecto     = MEDIANO (0.5 < d < 0.8)\n";
    else                         std::cout << "    Efecto     = PEQUENO (d < 0.5)\n";

    auto tt2 = t_test(st_sw, st_flex, K_TRIALS);
    std::cout << "\n  switch vs FlexBranch:\n";
    std::cout << "    t-stat     = " << std::setprecision(3) << tt2.t_stat << "\n";
    std::cout << "    Cohen's d  = " << std::setprecision(3) << tt2.cohen_d << "\n";
    std::cout << "    Resultado  = " << (tt2.significativo ? "SIGNIFICATIVO (p < 0.05)" : "NO significativo") << "\n";

    auto tt3 = t_test(st_fast, st_flex, K_TRIALS);
    std::cout << "\n  FastBranch vs FlexBranch:\n";
    std::cout << "    t-stat     = " << std::setprecision(3) << tt3.t_stat << "\n";
    std::cout << "    Cohen's d  = " << std::setprecision(3) << tt3.cohen_d << "\n";
    std::cout << "    Resultado  = " << (tt3.significativo ? "SIGNIFICATIVO (p < 0.05)" : "NO significativo") << "\n";

    auto tt4 = t_test(st_fast, st_dir, K_TRIALS);
    std::cout << "\n  FastBranch vs Directo (caso ideal sin branches):\n";
    std::cout << "    t-stat     = " << std::setprecision(3) << tt4.t_stat << "\n";
    std::cout << "    Cohen's d  = " << std::setprecision(3) << tt4.cohen_d << "\n";
    std::cout << "    Resultado  = " << (tt4.significativo ? "SIGNIFICATIVO (p < 0.05)" : "NO significativo") << "\n";

    // Histogramas
    std::cout << "\n\n  DISTRIBUCION DE TIEMPOS\n";
    std::cout << "  ================================================================\n\n";

    histograma(v_switch, "switch (aleatorio)");
    histograma(v_fast,   "FastBranch (set/1000)");
    histograma(v_speedup, "Speedup (switch/FastBranch)");

    // Resumen final
    std::cout << "\n  CONCLUSION\n";
    std::cout << "  ================================================================\n\n";

    std::cout << "  Sobre " << K_TRIALS << " trials independientes con semillas aleatorias:\n\n";

    std::cout << "  FastBranch vs switch:\n";
    std::cout << "    Speedup medio:    " << std::setprecision(2) << st_sp.media << "x\n";
    std::cout << "    IC 95% speedup:   [" << st_sp.ic95_low << "x, " << st_sp.ic95_high << "x]\n";
    std::cout << "    Ahorro medio:     " << std::setprecision(2) << (st_sw.media - st_fast.media)
              << " ms (" << std::setprecision(1) << (st_sw.media - st_fast.media)*1e6/N_OPS << " ns/op)\n";
    std::cout << "    Estadisticamente: " << (tt1.significativo ? "SI" : "NO") << " significativo\n";
    std::cout << "    Tamano efecto:    Cohen's d = " << std::setprecision(2) << tt1.cohen_d;
    if (tt1.cohen_d > 0.8) std::cout << " (GRANDE)\n";
    else std::cout << "\n";

    std::cout << "\n  Overhead std::function (FlexBranch vs FastBranch):\n";
    std::cout << "    Diferencia media: " << std::setprecision(2) << (st_flex.media - st_fast.media) << " ms\n";
    std::cout << "    Ratio:            " << std::setprecision(3) << (st_flex.media / st_fast.media) << "x\n";
    std::cout << "    Estadisticamente: " << (tt3.significativo ? "SI" : "NO") << " significativo\n";

    std::cout << "\n  FastBranch vs caso ideal (0 branches):\n";
    std::cout << "    Diferencia media: " << std::setprecision(2) << (st_fast.media - st_dir.media) << " ms\n";
    std::cout << "    Ratio:            " << std::setprecision(3) << (st_fast.media / st_dir.media) << "x\n";

    std::cout << "\n  ================================================================\n\n";

    return 0;
}
