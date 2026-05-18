/**
 * ============================================================
 *  SEMI-STATIC CONDITIONS + MULTI-THREADING
 *  Arquitecturas paralelas para HFT
 *
 *  Patron principal:
 *    - 1 hilo cold path: monitorea mercado, llama set_direction()
 *    - N hilos hot path: procesan ordenes, llaman branch() sin locks
 *
 *  Compilar:
 *    g++ -std=c++17 -O3 -o hft_mt.exe examples/semi_static_mt.cpp
 *    .\hft_mt.exe
 * ============================================================
 */

#include <iostream>
#include <chrono>
#include <random>
#include <string>
#include <iomanip>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <barrier>
#include <cassert>
#include <functional>
#include <algorithm>

// ============================================================
//  CONFIGURACION
// ============================================================

const int N_ORDENES    = 10'000'000;  // 10 millones de ordenes
const int N_CORES      = (int)std::thread::hardware_concurrency();
const int N_HOT_HILOS  = std::max(2, N_CORES - 1); // reservar 1 core para cold path

// ============================================================
//  PARTE 1: BRANCHCHANGER THREAD-SAFE (con atomic)
//
//  La version basica usaba un int normal: no segura entre hilos.
//  Esta version usa atomic<int>:
//    - set_direction(): escribe atomicamente (cold path thread)
//    - branch():        lee atomicamente  (hot path threads)
//    - Sin mutex: lectura/escritura nunca se "rompen" a mitad
//    - Peor caso: 1 orden ejecutada con direccion "vieja" durante cambio
//      (aceptable en HFT -- mejor que bloquear millones de ordenes)
// ============================================================

template <typename Ret, typename... Args>
class BranchChanger {
public:
    using FuncPtr = Ret(*)(Args...);

    template <typename... Funcs>
    explicit BranchChanger(Funcs... funcs)
        : ramas_({static_cast<FuncPtr>(funcs)...})
    {
        indice_actual_.store(0, std::memory_order_relaxed);
    }

    // COLD PATH: escribe atomicamente la nueva direccion
    // memory_order_release: garantiza que hot path threads ven el cambio
    void set_direction(int indice) {
        assert(indice >= 0 && indice < (int)ramas_.size());
        indice_actual_.store(indice, std::memory_order_release);
    }

    void set_direction(bool cond) {
        indice_actual_.store(cond ? 0 : 1, std::memory_order_release);
    }

    // HOT PATH: lee atomicamente y salta
    // memory_order_acquire: ve cambios del cold path thread
    Ret branch(Args... args) {
        int idx = indice_actual_.load(std::memory_order_acquire);
        return ramas_[idx](std::forward<Args>(args)...);
    }

    int num_ramas()   const { return (int)ramas_.size(); }
    int rama_activa() const { return indice_actual_.load(std::memory_order_relaxed); }

private:
    std::vector<FuncPtr>  ramas_;
    std::atomic<int>      indice_actual_;
    // Padding para evitar false sharing con otras variables en el mismo cache line
    char padding_[64 - sizeof(int)];
};

// ============================================================
//  PARTE 2: VERSION CON MUTEX (para comparar lo malo)
//
//  Cada llamada a branch() adquiere un lock.
//  Esto serializa completamente los hot path threads => sin paralelismo.
// ============================================================

template <typename Ret, typename... Args>
class BranchChangerMutex {
public:
    using FuncPtr = Ret(*)(Args...);

    template <typename... Funcs>
    explicit BranchChangerMutex(Funcs... funcs)
        : ramas_({static_cast<FuncPtr>(funcs)...}), indice_(0) {}

    void set_direction(int i) {
        std::lock_guard<std::mutex> lg(mtx_);
        indice_ = i;
    }

    Ret branch(Args... args) {
        std::lock_guard<std::mutex> lg(mtx_);   // ← esto destruye el rendimiento
        return ramas_[indice_](std::forward<Args>(args)...);
    }

private:
    std::vector<FuncPtr> ramas_;
    int                  indice_;
    std::mutex           mtx_;
};


// ============================================================
//  PARTE 3: FUNCIONES RAMA (4 exchanges, trabajo realista)
// ============================================================

// Contadores por exchange (para verificar que todos los hilos trabajan)
alignas(64) std::atomic<long long> contadores[4] = {};

void enviar_NYSE  (double p, int q) { contadores[0].fetch_add((long long)(p*q*1.000), std::memory_order_relaxed); }
void enviar_NASDAQ(double p, int q) { contadores[1].fetch_add((long long)(p*q*0.999), std::memory_order_relaxed); }
void enviar_BATS  (double p, int q) { contadores[2].fetch_add((long long)(p*q*0.998), std::memory_order_relaxed); }
void enviar_IEX   (double p, int q) { contadores[3].fetch_add((long long)(p*q*0.997), std::memory_order_relaxed); }

const char* EXCHANGE_NAMES[] = {"NYSE", "NASDAQ", "BATS", "IEX"};

void reset_contadores() {
    for (auto& c : contadores) c.store(0, std::memory_order_relaxed);
}

// ============================================================
//  PARTE 4: BENCHMARKS
// ============================================================

struct ResultadoBench {
    std::string nombre;
    double      tiempo_ms;
    int         n_hilos;
};

using Bench = BranchChanger<void, double, int>;

// ------------ PATRON A: 1 hilo, sin paralelismo (baseline) ------------
// El mismo benchmark que antes, referencia para medir el speedup
ResultadoBench bench_1_hilo_aleatorio() {
    reset_contadores();
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, 3);
    std::vector<int> conds(N_ORDENES);
    for (auto& c : conds) c = dist(rng);

    Bench router(enviar_NYSE, enviar_NASDAQ, enviar_BATS, enviar_IEX);

    auto t0 = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < N_ORDENES; i++) {
        if (i % 1000 == 0) router.set_direction(conds[i]);  // cold
        router.branch(150.0 + (i%10), 100 + (i%50));        // hot
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    return {"Semi-static  1 hilo  (referencia)",
            std::chrono::duration<double,std::milli>(t1-t0).count(), 1};
}


// ------------ PATRON B: N hilos hot path, trabajo particionado ------------
//
//  Idea: dividir los 10M de ordenes en N partes iguales.
//  Cada hilo tiene su propio BranchChanger (sin compartir estado).
//  La condicion se evalua en el hilo principal antes de lanzar los hilos.
//
//  Diagrama:
//    Main  : set_direction(cond)  ----------->  lanza N hilos
//    Hilo1 :                      branch() branch() branch() ...
//    Hilo2 :                      branch() branch() branch() ...
//    HiloN :                      branch() branch() branch() ...
//    Main  : espera a todos (join)  ----------->  fin
//
ResultadoBench bench_N_hilos_particionado(int n_hilos) {
    reset_contadores();
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, 3);
    std::vector<int> conds(N_ORDENES);
    for (auto& c : conds) c = dist(rng);

    auto t0 = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> hilos;
    int chunk = N_ORDENES / n_hilos;

    for (int t = 0; t < n_hilos; t++) {
        int inicio = t * chunk;
        int fin    = (t == n_hilos - 1) ? N_ORDENES : inicio + chunk;

        hilos.emplace_back([&conds, inicio, fin]() {
            // Cada hilo tiene su propia instancia de BranchChanger
            Bench router(enviar_NYSE, enviar_NASDAQ, enviar_BATS, enviar_IEX);

            for (int i = inicio; i < fin; i++) {
                if (i % 1000 == 0) router.set_direction(conds[i]);  // cold
                router.branch(150.0 + (i%10), 100 + (i%50));        // hot
            }
        });
    }

    for (auto& h : hilos) h.join();

    auto t1 = std::chrono::high_resolution_clock::now();
    return {"Semi-static  " + std::to_string(n_hilos) + " hilos particionados",
            std::chrono::duration<double,std::milli>(t1-t0).count(), n_hilos};
}


// ------------ PATRON C: 1 cold hilo + N hot hilos (modelo real HFT) ------------
//
//  Este es el patron del paper: separacion total de cold y hot path.
//
//  Diagrama:
//    Cold hilo: [evalua mercado] -> set_direction(X) -> duerme 1ms -> repite
//    Hot hilo1: branch() branch() branch() branch() branch() ...
//    Hot hilo2: branch() branch() branch() branch() branch() ...
//    Hot hiloN: branch() branch() branch() branch() branch() ...
//
//  BranchChanger compartido con atomic<int> -- sin mutex en hot path.
//
ResultadoBench bench_cold_hot_separados(int n_hot_hilos) {
    reset_contadores();

    // BranchChanger COMPARTIDO entre todos los hilos
    Bench router(enviar_NYSE, enviar_NASDAQ, enviar_BATS, enviar_IEX);

    std::atomic<bool>     running{true};
    std::atomic<long long> ordenes_total{0};

    // -- COLD PATH THREAD: cambia la direccion cada ~1000 ordenes procesadas --
    std::thread cold_thread([&]() {
        std::mt19937 rng(99);
        std::uniform_int_distribution<int> dist(0, 3);
        long long ultima_actualizacion = 0;

        while (running.load(std::memory_order_relaxed)) {
            long long actual = ordenes_total.load(std::memory_order_relaxed);
            if (actual - ultima_actualizacion >= 1000) {
                router.set_direction(dist(rng));   // ← cold path: costoso pero infrecuente
                ultima_actualizacion = actual;
            }
            // En produccion real: este hilo también procesaria datos de red (market feed)
            std::this_thread::yield();
        }
    });

    auto t0 = std::chrono::high_resolution_clock::now();

    // -- HOT PATH THREADS: procesan ordenes sin ningún lock --
    std::vector<std::thread> hot_threads;
    int chunk = N_ORDENES / n_hot_hilos;

    for (int t = 0; t < n_hot_hilos; t++) {
        int inicio = t * chunk;
        int fin    = (t == n_hot_hilos - 1) ? N_ORDENES : inicio + chunk;

        hot_threads.emplace_back([&router, &ordenes_total, inicio, fin]() {
            int local_count = 0;
            for (int i = inicio; i < fin; i++) {
                // HOT PATH: jmp directo, sin check, sin lock
                router.branch(150.0 + (i%10), 100 + (i%50));
                local_count++;
                // Reportar progreso al cold thread cada 10000 ordenes
                if (local_count % 10000 == 0)
                    ordenes_total.fetch_add(10000, std::memory_order_relaxed);
            }
        });
    }

    for (auto& h : hot_threads) h.join();

    auto t1 = std::chrono::high_resolution_clock::now();
    running.store(false);
    cold_thread.join();

    return {"Cold+Hot " + std::to_string(n_hot_hilos) + " hilos (patron real HFT)",
            std::chrono::duration<double,std::milli>(t1-t0).count(), n_hot_hilos + 1};
}


// ------------ PATRON D: Con mutex (para mostrar lo malo) ------------
ResultadoBench bench_mutex_compartido(int n_hilos) {
    reset_contadores();

    BranchChangerMutex<void, double, int> router(
        enviar_NYSE, enviar_NASDAQ, enviar_BATS, enviar_IEX);
    router.set_direction(0);

    auto t0 = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> hilos;
    int chunk = N_ORDENES / n_hilos;

    for (int t = 0; t < n_hilos; t++) {
        int inicio = t * chunk;
        int fin    = (t == n_hilos - 1) ? N_ORDENES : inicio + chunk;

        hilos.emplace_back([&router, inicio, fin]() {
            for (int i = inicio; i < fin; i++) {
                router.branch(150.0 + (i%10), 100 + (i%50));  // ← adquiere mutex cada vez
            }
        });
    }

    for (auto& h : hilos) h.join();

    auto t1 = std::chrono::high_resolution_clock::now();
    return {"Con MUTEX " + std::to_string(n_hilos) + " hilos (malo: contention)",
            std::chrono::duration<double,std::milli>(t1-t0).count(), n_hilos};
}


// ============================================================
//  PARTE 5: DIAGRAMA DEL PATRON HFT REAL
// ============================================================

void imprimir_diagrama() {
    std::cout << "\n";
    std::cout << "  PATRON MULTI-HILO OPTIMO PARA HFT\n";
    std::cout << "  ==================================\n\n";
    std::cout << "  Core 0 (COLD PATH):                                     \n";
    std::cout << "  +-------------------------------------------------+     \n";
    std::cout << "  | Market Monitor Thread                           |     \n";
    std::cout << "  | [recibe feed] -> evalua() -> set_direction(X)  |     \n";
    std::cout << "  | [atomic write, memory_order_release]            |     \n";
    std::cout << "  +-------------------------------------------------+     \n";
    std::cout << "         |  atomic<int> compartido                        \n";
    std::cout << "         |  (sin mutex, sin lock)                         \n";
    std::cout << "  +------+-------+-------+-------+                        \n";
    std::cout << "  |      |       |       |       |                        \n";
    std::cout << "  v      v       v       v       v                        \n";
    std::cout << "  Core1  Core2  Core3  Core4  Core5 ... (HOT PATH)\n";
    std::cout << "  +----+ +----+ +----+ +----+ +----+\n";
    std::cout << "  |hot | |hot | |hot | |hot | |hot |\n";
    std::cout << "  |thr | |thr | |thr | |thr | |thr |\n";
    std::cout << "  |jmp | |jmp | |jmp | |jmp | |jmp |\n";
    std::cout << "  |jmp | |jmp | |jmp | |jmp | |jmp |\n";
    std::cout << "  |jmp | |jmp | |jmp | |jmp | |jmp |\n";
    std::cout << "  +----+ +----+ +----+ +----+ +----+\n\n";
    std::cout << "  Cada hot thread lee el atomic<int> (memory_order_acquire)\n";
    std::cout << "  y ejecuta branch() sin ningun bloqueo.\n";
    std::cout << "  El peor caso: 1 orden ejecutada con direccion vieja\n";
    std::cout << "  durante la transicion. Aceptable en HFT.\n";
}

void imprimir_tabla(const std::vector<ResultadoBench>& resultados) {
    double base = resultados[0].tiempo_ms;

    std::cout << "\n  RESULTADOS (" << N_ORDENES/1'000'000 << "M ordenes, "
              << N_CORES << " cores disponibles)\n";
    std::cout << "  +------------------------------------------+-------+--------+----------+\n";
    std::cout << "  | Enfoque                                  |Hilos  | Tiempo | Speedup  |\n";
    std::cout << "  +------------------------------------------+-------+--------+----------+\n";

    for (const auto& r : resultados) {
        double speedup = base / r.tiempo_ms;
        std::cout << "  | " << std::left  << std::setw(40) << r.nombre << " | "
                  << std::right << std::setw(5) << r.n_hilos << " | "
                  << std::setw(5) << std::fixed << std::setprecision(0) << r.tiempo_ms << " ms | "
                  << std::setw(6) << std::setprecision(2) << speedup << "x    |\n";
    }
    std::cout << "  +------------------------------------------+-------+--------+----------+\n";
    std::cout << "  Speedup = tiempo_1_hilo / tiempo_N_hilos\n";
}

void imprimir_memory_order() {
    std::cout << "\n";
    std::cout << "  POR QUE ATOMIC Y NO MUTEX\n";
    std::cout << "  ==========================\n\n";
    std::cout << "  MUTEX: solo 1 hilo puede estar en branch() a la vez\n";
    std::cout << "  -------------------------------------------------------\n";
    std::cout << "  Hilo1: [lock] branch() [unlock]\n";
    std::cout << "  Hilo2:        [espera...]       [lock] branch() [unlock]\n";
    std::cout << "  => Los hilos se bloquean entre si. Sin paralelismo real.\n\n";
    std::cout << "  ATOMIC: todos los hilos ejecutan branch() en paralelo\n";
    std::cout << "  -------------------------------------------------------\n";
    std::cout << "  Hilo1: branch()  branch()  branch()  ...\n";
    std::cout << "  Hilo2: branch()  branch()  branch()  ...\n";
    std::cout << "  Hilo3: branch()  branch()  branch()  ...\n";
    std::cout << "  ColdT:           set_direction(X)             (infrecuente)\n";
    std::cout << "  => Verdadero paralelismo. Solo 1 instruccion atomic load.\n\n";
    std::cout << "  memory_order_release en set_direction():\n";
    std::cout << "    Garantiza que los hot threads ven el nuevo indice.\n";
    std::cout << "  memory_order_acquire en branch():\n";
    std::cout << "    Garantiza que lee el valor mas reciente escrito.\n";
}


// ============================================================
//  MAIN
// ============================================================

int main() {
    std::cout << "\n";
    std::cout << "  ================================================================\n";
    std::cout << "  SEMI-STATIC + MULTI-THREADING -- Benchmark HFT\n";
    std::cout << "  Basado en: Bilokon, Lucuta & Shermer (JPDC 2025)\n";
    std::cout << "  ================================================================\n";

    std::cout << "\n  Cores disponibles en este sistema: " << N_CORES << "\n";
    std::cout << "  Hilos hot path usados: " << N_HOT_HILOS << "\n\n";

    // Diagrama primero
    imprimir_diagrama();

    // Benchmarks
    std::cout << "\n  Ejecutando benchmarks (" << N_ORDENES/1'000'000 << "M ordenes)...\n\n";

    std::vector<ResultadoBench> resultados;

    auto r0 = bench_1_hilo_aleatorio();
    std::cout << "  [1/" << 4+N_HOT_HILOS << "] " << r0.nombre << ": " << r0.tiempo_ms << " ms\n";
    resultados.push_back(r0);

    for (int n : {2, N_HOT_HILOS}) {
        auto r = bench_N_hilos_particionado(n);
        std::cout << "  [?/" << 4+N_HOT_HILOS << "] " << r.nombre << ": " << r.tiempo_ms << " ms\n";
        resultados.push_back(r);
    }

    auto r_hft = bench_cold_hot_separados(N_HOT_HILOS);
    std::cout << "  [?/" << 4+N_HOT_HILOS << "] " << r_hft.nombre << ": " << r_hft.tiempo_ms << " ms\n";
    resultados.push_back(r_hft);

    auto r_mtx = bench_mutex_compartido(N_HOT_HILOS);
    std::cout << "  [?/" << 4+N_HOT_HILOS << "] " << r_mtx.nombre << ": " << r_mtx.tiempo_ms << " ms\n";
    resultados.push_back(r_mtx);

    // Tabla
    imprimir_tabla(resultados);

    // Explicacion del atomic
    imprimir_memory_order();

    std::cout << "\n  ================================================================\n\n";
    return 0;
}
