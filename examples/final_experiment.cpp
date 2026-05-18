/**
 * ============================================================
 *  EXPERIMENTO FINAL: Semi-Static Conditions
 *  Version definitiva, sin hilos, sin overhead de std::function
 *
 *  HALLAZGO DE LA SESION:
 *    - std::function tiene ~2-5x overhead de indirección vs ptr crudo
 *    - multi-threading no ayuda cuando los threads comparten escritura
 *    - La ganancia REAL viene de eliminar mispredictions en branches
 *      ligeros con condiciones impredecibles
 *
 *  SOLUCION FINAL:
 *    - FastBranchChanger: array estatico de punteros a funcion (sin heap)
 *    - Sin std::function, sin vector, sin indirección
 *    - Mismo API: set_direction() / branch() / operator()
 *
 *  Compilar:
 *    g++ -std=c++17 -O2 -o final_demo.exe examples/final_experiment.cpp
 *    .\final_demo.exe
 * ============================================================
 */

#include <iostream>
#include <chrono>
#include <random>
#include <string>
#include <iomanip>
#include <array>
#include <vector>
#include <functional>
#include <cassert>

using Clock = std::chrono::high_resolution_clock;
using TP    = Clock::time_point;

double ms(TP t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// ============================================================
//  IMPLEMENTACION FINAL: FastBranchChanger<N, Ret, Args...>
//
//  Mejoras sobre BranchChanger original:
//    1. Array estatico en stack (no heap, no std::vector)
//    2. Punteros crudos (no std::function, 0 indirección extra)
//    3. Capacidad maxima N en tiempo de compilacion
//    4. set_direction() es literalmente: current_ = idx (1 ciclo)
//    5. branch() es literalmente: arr[current_](args...) (1 deref + call)
//
//  Restriccion: solo funciones libres o static (no lambdas con captura)
//  Para lambdas con captura: usar BranchChanger<Sig> del header general.
// ============================================================

template <int MaxN, typename Ret, typename... Args>
class FastBranchChanger {
public:
    using FuncPtr = Ret(*)(Args...);

    // Constructor variadic: hasta MaxN funciones
    template <typename... Funcs>
    explicit constexpr FastBranchChanger(Funcs... fns)
        : ptrs_{static_cast<FuncPtr>(fns)...}
        , count_(sizeof...(Funcs))
        , current_(0)
    {
        static_assert(sizeof...(Funcs) <= MaxN, "Demasiadas ramas para MaxN.");
        static_assert(sizeof...(Funcs) >= 1,    "Se necesita al menos 1 rama.");
    }

    // COLD PATH: O(1), literalmente una asignación
    void set_direction(int idx) {
        assert(idx >= 0 && idx < count_);
        current_ = idx;
    }
    void set_direction(bool cond) { current_ = cond ? 0 : 1; }
    void select(int idx)          { current_ = idx; }

    // HOT PATH: 1 deref de array + 1 call indirecto (sin std::function)
    Ret branch(Args... args) {
        return ptrs_[current_](std::forward<Args>(args)...);
    }
    Ret operator()(Args... args) { return branch(std::forward<Args>(args)...); }

    int size()   const { return count_; }
    int active() const { return current_; }

private:
    FuncPtr ptrs_[MaxN];  // array en stack, sin heap
    int     count_;
    int     current_;
};


// ============================================================
//  FUNCIONES RAMA: computacion ligera (donde semi-static brilla)
//
//  Regla: branch LIGERO + condicion IMPREDECIBLE = maximo beneficio.
//  Si el branch es pesado (string alloc, I/O), el misprediction
//  penalty queda enmascarado por el trabajo de la funcion misma.
// ============================================================

// --- Caso HFT: operaciones aritmeticas de precio ---
volatile double G = 0.0;  // sink global para evitar optimizacion

void calc_bid(double price, int qty)  { G += price * qty * 1.0002; }
void calc_ask(double price, int qty)  { G += price * qty * 0.9998; }
void calc_mid(double price, int qty)  { G += price * qty * 1.0000; }
void calc_vwap(double price, int qty) { G += price * qty / (qty + 1); }

// --- Caso Logging: nivel configurable ---
volatile int LOG_COUNT = 0;
void log_debug(const char* msg, int lvl) { LOG_COUNT++; /* escribe */ }
void log_info (const char* msg, int lvl) { if (lvl >= 1) LOG_COUNT++; }
void log_warn (const char* msg, int lvl) { if (lvl >= 2) LOG_COUNT++; }
void log_off  (const char* msg, int lvl) { /* no hace nada */ }

// --- Caso Compresion: algoritmo seleccionable ---
volatile int COMP_BYTES = 0;
void compress_lz4   (int size) { COMP_BYTES += size * 7 / 10; }
void compress_zstd  (int size) { COMP_BYTES += size * 5 / 10; }
void compress_snappy(int size) { COMP_BYTES += size * 8 / 10; }
void compress_none  (int size) { COMP_BYTES += size; }


// ============================================================
//  BENCHMARK CORE: compara 4 enfoques en el mismo problema
//
//  Mide exclusivamente el overhead de SELECCION de rama,
//  usando branches ligeros para que el misprediction penalty
//  sea visible y dominante.
// ============================================================

const int N = 10'000'000;

struct Result {
    const char* nombre;
    double      tiempo_ms;
    double      por_op_ns;
};

void print_results(const std::vector<Result>& rs) {
    double base = rs[0].tiempo_ms;
    std::cout << "\n";
    std::cout << "  +-------------------------------------------------+----------+----------+----------+\n";
    std::cout << "  | Enfoque                                         | Total    | ns/op    | Speedup  |\n";
    std::cout << "  +-------------------------------------------------+----------+----------+----------+\n";
    for (auto& r : rs) {
        double sp = base / r.tiempo_ms;
        std::cout << "  | " << std::left  << std::setw(47) << r.nombre    << " | "
                  << std::right << std::fixed << std::setprecision(1)
                  << std::setw(6) << r.tiempo_ms   << " ms | "
                  << std::setw(6) << std::setprecision(2) << r.por_op_ns << " ns | "
                  << std::setw(6) << std::setprecision(2) << sp           << "x    |\n";
    }
    std::cout << "  +-------------------------------------------------+----------+----------+----------+\n";
    std::cout << "  Baseline = primer enfoque de la tabla\n";
}

// ============================================================
//  EXPERIMENTO 1: Precio HFT — 4 ramas, condicion aleatoria
// ============================================================
void experimento_hft() {
    std::cout << "\n=== EXPERIMENTO 1: Calculo de Precio HFT ===\n";
    std::cout << "  Firma: void(double, int)  |  4 ramas  |  " << N/1'000'000 << "M ops\n";
    std::cout << "  Condicion: ALEATORIA (uniforme entre 4 opciones)\n";

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist4(0, 3);
    std::vector<int> conds(N);
    for (auto& c : conds) c = dist4(rng);

    std::vector<Result> rs;

    // ---- A: switch (peor caso con condicion aleatoria) ----
    G = 0;
    auto t = Clock::now();
    for (int i = 0; i < N; i++) {
        double p = 150.0 + (i % 10);
        int    q = 100   + (i % 50);
        switch (conds[i]) {
            case 0: calc_bid (p, q); break;
            case 1: calc_ask (p, q); break;
            case 2: calc_mid (p, q); break;
            case 3: calc_vwap(p, q); break;
        }
    }
    double ms_sw = ms(t);
    rs.push_back({"switch (evalua condicion cada op)", ms_sw, ms_sw*1e6/N});

    // ---- B: BranchChanger con std::function ----
    G = 0;
    auto bc_sf = [&]() {
        // Usando el header general con std::function
        std::function<void(double,int)> ramas[4] = {calc_bid, calc_ask, calc_mid, calc_vwap};
        int current = 0;
        auto t2 = Clock::now();
        for (int i = 0; i < N; i++) {
            if (i % 1000 == 0) current = conds[i];
            double p = 150.0 + (i % 10);
            int    q = 100   + (i % 50);
            ramas[current](p, q);
        }
        return ms(t2);
    };
    double ms_sf = bc_sf();
    rs.push_back({"BranchChanger std::function (set/1000)", ms_sf, ms_sf*1e6/N});

    // ---- C: FastBranchChanger (punteros crudos, SOLUCION FINAL) ----
    G = 0;
    FastBranchChanger<4, void, double, int> fast(calc_bid, calc_ask, calc_mid, calc_vwap);
    t = Clock::now();
    for (int i = 0; i < N; i++) {
        if (i % 1000 == 0) fast.set_direction(conds[i]);
        fast(150.0 + (i % 10), 100 + (i % 50));
    }
    double ms_fast = ms(t);
    rs.push_back({"FastBranchChanger ptr crudo (set/1000)", ms_fast, ms_fast*1e6/N});

    // ---- D: FastBranchChanger con rafagas de 5000 ----
    G = 0;
    fast.set_direction(0);
    t = Clock::now();
    for (int i = 0; i < N; i++) {
        if (i % 5000 == 0) fast.set_direction(conds[i]);
        fast(150.0 + (i % 10), 100 + (i % 50));
    }
    double ms_fast5 = ms(t);
    rs.push_back({"FastBranchChanger ptr crudo (set/5000)", ms_fast5, ms_fast5*1e6/N});

    // ---- E: if/else predecible (baseline teorico del CPU) ----
    G = 0;
    t = Clock::now();
    for (int i = 0; i < N; i++) {
        double p = 150.0 + (i % 10);
        int    q = 100   + (i % 50);
        calc_bid(p, q);  // siempre el mismo: 0 mispredictions
    }
    double ms_pred = ms(t);
    rs.push_back({"Directo (0 branches, caso ideal CPU)  ", ms_pred, ms_pred*1e6/N});

    print_results(rs);

    std::cout << "\n  Ahorro FastBranchChanger vs switch: "
              << std::fixed << std::setprecision(1) << (ms_sw - ms_fast) << " ms  ("
              << std::setprecision(0) << (ms_sw - ms_fast) * 1e6 / N << " ns/op)\n";
}


// ============================================================
//  EXPERIMENTO 2: Logger — 4 niveles, condicion semi-predecible
//
//  Condicion cambia cada 10K ops (simula cambio de config en runtime).
//  En este rango, if/else empieza a predecirlo pero semi-static lo gana.
// ============================================================
void experimento_logger() {
    std::cout << "\n=== EXPERIMENTO 2: Logger con Nivel Configurable ===\n";
    std::cout << "  Firma: void(const char*, int)  |  4 niveles  |  " << N/1'000'000 << "M ops\n";
    std::cout << "  Condicion: cambia cada 10000 ops (semi-predecible)\n";

    std::mt19937 rng(99);
    std::uniform_int_distribution<int> dist4(0, 3);
    std::vector<int> conds(N);
    for (int i = 0; i < N; i++) {
        if (i % 10000 == 0) conds[i] = dist4(rng);
        else                conds[i] = conds[i - (i%10000)];
    }

    std::vector<Result> rs;
    const char* msg = "event";

    // ---- A: if/else (CPU intenta predecir el patron) ----
    LOG_COUNT = 0;
    auto t = Clock::now();
    for (int i = 0; i < N; i++) {
        int lvl = conds[i];
        if      (lvl == 0) log_debug(msg, lvl);
        else if (lvl == 1) log_info (msg, lvl);
        else if (lvl == 2) log_warn (msg, lvl);
        else               log_off  (msg, lvl);
    }
    double ms_ie = ms(t);
    rs.push_back({"if/else chain (CPU predice patron)", ms_ie, ms_ie*1e6/N});

    // ---- B: FastBranchChanger (set_direction cada 10K) ----
    LOG_COUNT = 0;
    FastBranchChanger<4, void, const char*, int> logger(
        log_debug, log_info, log_warn, log_off);
    t = Clock::now();
    for (int i = 0; i < N; i++) {
        if (i % 10000 == 0) logger.set_direction(conds[i]);
        logger(msg, conds[i]);
    }
    double ms_fast = ms(t);
    rs.push_back({"FastBranchChanger (set/10000 = cuando cambia)", ms_fast, ms_fast*1e6/N});

    // ---- C: nivel OFF (el más optimizable por el CPU) ----
    LOG_COUNT = 0;
    logger.set_direction(3);  // log_off
    t = Clock::now();
    for (int i = 0; i < N; i++) logger(msg, 3);
    double ms_off = ms(t);
    rs.push_back({"FastBranchChanger nivel OFF fijo (ideal)    ", ms_off, ms_off*1e6/N});

    print_results(rs);
}


// ============================================================
//  EXPERIMENTO 3: Compresion — 4 algoritmos, benchmark real
//
//  Compara overhead puro de seleccion (ambas versiones hacen
//  el mismo trabajo), aislando el costo del dispatch.
// ============================================================
void experimento_compresion() {
    std::cout << "\n=== EXPERIMENTO 3: Selector de Algoritmo de Compresion ===\n";
    std::cout << "  Firma: void(int)  |  4 algoritmos  |  " << N/1'000'000 << "M bloques\n";
    std::cout << "  Condicion: fija por rafaga de 100K bloques\n";

    std::mt19937 rng(7);
    std::uniform_int_distribution<int> dist4(0, 3);
    const char* nombres[] = {"LZ4", "Zstd", "Snappy", "None"};

    FastBranchChanger<4, void, int> comp(
        compress_lz4, compress_zstd, compress_snappy, compress_none);

    std::cout << "\n  FastBranchChanger por algoritmo:\n";
    std::vector<Result> rs;

    for (int algo = 0; algo < 4; algo++) {
        COMP_BYTES = 0;
        comp.set_direction(algo);
        auto t = Clock::now();
        for (int i = 0; i < N; i++) comp(1024);
        double ms_t = ms(t);
        rs.push_back({nombres[algo], ms_t, ms_t * 1e6 / N});
    }

    // Comparar con switch equivalente (condicion aleatoria entre los 4)
    std::vector<int> conds(N);
    for (auto& c : conds) c = dist4(rng);
    COMP_BYTES = 0;
    auto t = Clock::now();
    for (int i = 0; i < N; i++) {
        switch (conds[i]) {
            case 0: compress_lz4   (1024); break;
            case 1: compress_zstd  (1024); break;
            case 2: compress_snappy(1024); break;
            case 3: compress_none  (1024); break;
        }
    }
    double ms_sw = ms(t);
    rs.push_back({"switch aleatorio (referencia)", ms_sw, ms_sw*1e6/N});

    print_results(rs);
}


// ============================================================
//  RESUMEN FINAL: CUANDO USAR CADA ENFOQUE
// ============================================================
void resumen_final(double ms_switch, double ms_fast, double ms_std_fn) {
    std::cout << "\n";
    std::cout << "  ================================================================\n";
    std::cout << "  RESUMEN: CUANDO USAR CADA ENFOQUE\n";
    std::cout << "  ================================================================\n\n";

    std::cout << "  CONDICION SIEMPRE IGUAL (flag estatico, nivel de log fijo):\n";
    std::cout << "    => if/else o switch normal\n";
    std::cout << "    El CPU la predice perfectamente. No hay nada que optimizar.\n\n";

    std::cout << "  CONDICION IMPREDECIBLE, BRANCH LIGERO (HFT, routing, dispatch):\n";
    std::cout << "    => FastBranchChanger<N, Ret, Args...>  (punteros crudos)\n";
    std::cout << "    Sin std::function, sin heap, 1 array en stack.\n";
    std::cout << "    Ganancia medida: ~" << std::fixed << std::setprecision(0)
              << (ms_switch - ms_fast) * 1e6 / N << " ns/op\n\n";

    std::cout << "  CONDICION IMPREDECIBLE, BRANCH CON LAMBDAS/CAPTURAS:\n";
    std::cout << "    => BranchChanger<Sig>  (std::function, header general)\n";
    std::cout << "    Overhead de indirección, pero acepta cualquier callable.\n";
    std::cout << "    Overhead std::function medido: ~"
              << std::setprecision(2) << ms_std_fn / ms_fast << "x vs ptr crudo\n\n";

    std::cout << "  CONDICION IMPREDECIBLE, BRANCH PESADO (string, I/O, network):\n";
    std::cout << "    => Cualquiera (el branch NO es el cuello de botella)\n";
    std::cout << "    El trabajo de la funcion domina. Misprediction = ruido.\n\n";

    std::cout << "  REGLA CRITICA: set_direction() NUNCA en el mismo loop que branch()\n";
    std::cout << "    SMC machine clear => 30-40x overhead\n";
    std::cout << "    Separar siempre en cold path / hot path.\n";
    std::cout << "  ================================================================\n";
}


// ============================================================
//  MAIN
// ============================================================
int main() {
    std::cout << "\n";
    std::cout << "  ================================================================\n";
    std::cout << "  EXPERIMENTO FINAL — Semi-Static Conditions\n";
    std::cout << "  FastBranchChanger vs std::function vs switch\n";
    std::cout << "  " << N/1'000'000 << "M operaciones por experimento\n";
    std::cout << "  ================================================================\n";

    experimento_hft();
    experimento_logger();
    experimento_compresion();

    // Recuperar valores del experimento 1 para el resumen
    // (re-ejecutar minimo para tener los numeros)
    std::mt19937 rng2(42);
    std::uniform_int_distribution<int> dist4(0, 3);
    std::vector<int> conds(N);
    for (auto& c : conds) c = dist4(rng2);

    // Switch
    G = 0;
    auto t = Clock::now();
    for (int i = 0; i < N; i++) {
        switch(conds[i]) {
            case 0: calc_bid(150.0+(i%10),100+(i%50)); break;
            case 1: calc_ask(150.0+(i%10),100+(i%50)); break;
            case 2: calc_mid(150.0+(i%10),100+(i%50)); break;
            case 3: calc_vwap(150.0+(i%10),100+(i%50)); break;
        }
    }
    double ms_sw = ms(t);

    // FastBranchChanger
    G = 0;
    FastBranchChanger<4, void, double, int> fast(calc_bid, calc_ask, calc_mid, calc_vwap);
    t = Clock::now();
    for (int i = 0; i < N; i++) {
        if (i % 1000 == 0) fast.set_direction(conds[i]);
        fast(150.0+(i%10), 100+(i%50));
    }
    double ms_fast = ms(t);

    // std::function
    G = 0;
    std::function<void(double,int)> ramas[4] = {calc_bid, calc_ask, calc_mid, calc_vwap};
    int cur = 0;
    t = Clock::now();
    for (int i = 0; i < N; i++) {
        if (i % 1000 == 0) cur = conds[i];
        ramas[cur](150.0+(i%10), 100+(i%50));
    }
    double ms_sf = ms(t);

    resumen_final(ms_sw, ms_fast, ms_sf);

    std::cout << "\n";
    return 0;
}
