/**
 * ============================================================
 *  SEMI-STATIC CONDITIONS vs. IF/ELSE TRADICIONAL
 *  Ejemplo didactico con benchmark comparativo
 *
 *  BranchChanger GENERALIZADO: soporta N funciones (no solo 2)
 *  equivalente a un switch de N casos.
 *
 *  Compilar en Windows (PowerShell):
 *    g++ -std=c++17 -O3 -o hft_demo.exe examples/semi_static_hft.cpp
 *    .\hft_demo.exe
 *
 *  Compilar en Linux:
 *    g++ -std=c++17 -O3 -o hft_demo examples/semi_static_hft.cpp
 *    ./hft_demo
 * ============================================================
 */

#include <iostream>
#include <chrono>
#include <random>
#include <string>
#include <iomanip>
#include <vector>
#include <functional>
#include <cassert>

// ============================================================
//  PARTE 1: BranchChanger GENERALIZADO (N funciones)
//
//  Uso:
//    BranchChanger<void, double, int> router(fnA, fnB, fnC);
//    router.set_direction(indice);   // 0, 1, 2, ...
//    router.branch(precio, cantidad);
//
//  Equivale a:
//    switch (indice) {
//      case 0: fnA(precio, cantidad); break;
//      case 1: fnB(precio, cantidad); break;
//      case 2: fnC(precio, cantidad); break;
//    }
//
//  La diferencia: con semi-static, la evaluacion del indice
//  ocurre en el cold path. En el hot path solo hay un jmp.
// ============================================================

template <typename Ret, typename... Args>
class BranchChanger {
public:
    using FuncPtr = Ret(*)(Args...);

    /**
     * Constructor variadic: acepta cualquier cantidad de funciones rama.
     * Todas deben tener la misma firma (Ret, Args...).
     *
     * Ejemplo con 2 funciones (binario, como if/else):
     *   BranchChanger<void, Order> b(fn_exchange_A, fn_exchange_B);
     *
     * Ejemplo con 4 funciones (generalizado, como switch):
     *   BranchChanger<void, Order> b(fn_A, fn_B, fn_C, fn_D);
     */
    template <typename... Funcs>
    explicit BranchChanger(Funcs... funcs)
        : ramas_({static_cast<FuncPtr>(funcs)...}), indice_actual_(0)
    {
        static_assert(sizeof...(Funcs) >= 2,
            "BranchChanger necesita al menos 2 funciones rama.");
    }

    /**
     * COLD PATH: selecciona la rama activa.
     * Se llama con baja frecuencia (cuando cambian las condiciones).
     *
     * @param indice  0 = primera funcion, 1 = segunda, etc.
     *                Para compatibilidad con if/else: true=1, false=0
     */
    void set_direction(int indice) {
        assert(indice >= 0 && indice < (int)ramas_.size());
        indice_actual_ = indice;
    }

    // Sobrecarga para bool (compatibilidad directa con condiciones binarias)
    void set_direction(bool condicion) {
        indice_actual_ = condicion ? 0 : 1;
    }

    /**
     * HOT PATH: ejecuta la rama pre-seleccionada.
     * En la implementacion real del paper: es un "jmp [direccion_fija]"
     * sin ningun check ni comparacion en tiempo de ejecucion.
     *
     * @param args  Argumentos que se pasan a la funcion seleccionada.
     */
    Ret branch(Args... args) {
        // Produccion real: "asm jmp 0x<offset pre-computado>"
        // Aqui: emulamos el mismo comportamiento logico
        return ramas_[indice_actual_](std::forward<Args>(args)...);
    }

    int num_ramas()     const { return (int)ramas_.size(); }
    int rama_activa()   const { return indice_actual_; }

private:
    std::vector<FuncPtr> ramas_;
    int                  indice_actual_;
};


// ============================================================
//  PARTE 2: DOMINIO HFT -- Funciones rama
//
//  Ahora generalizamos a 4 exchanges (no solo 2).
//  BranchChanger actua como un switch de 4 casos.
// ============================================================

volatile long long resultado_global = 0;

// --- 4 exchanges posibles ---
void enviar_NYSE(double precio, int cantidad) {
    resultado_global += (long long)(precio * cantidad * 1.000);
}
void enviar_NASDAQ(double precio, int cantidad) {
    resultado_global += (long long)(precio * cantidad * 0.999);
}
void enviar_BATS(double precio, int cantidad) {
    resultado_global += (long long)(precio * cantidad * 0.998);
}
void enviar_IEX(double precio, int cantidad) {
    resultado_global += (long long)(precio * cantidad * 0.997);
}

const char* nombres_exchanges[] = { "NYSE", "NASDAQ", "BATS", "IEX" };


// ============================================================
//  PARTE 3: BENCHMARKS
// ============================================================

const int N_ORDENES = 5'000'000;

struct ResultadoBench {
    std::string nombre;
    double      tiempo_ms;
};

// Helper: genera condiciones aleatorias en rango [0, n)
std::vector<int> generar_condiciones(int n, int semilla = 42) {
    std::mt19937 rng(semilla);
    std::uniform_int_distribution<int> dist(0, n - 1);
    std::vector<int> v(N_ORDENES);
    for (auto& x : v) x = dist(rng);
    return v;
}

// ---- A: switch tradicional (4 casos, condicion aleatoria) ----
ResultadoBench bench_switch_aleatorio() {
    resultado_global = 0;
    auto conds = generar_condiciones(4);

    auto t0 = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < N_ORDENES; i++) {
        double precio   = 150.0 + (i % 10);
        int    cantidad = 100   + (i % 50);

        switch (conds[i]) {  // el CPU no puede predecir cual caso es
            case 0: enviar_NYSE  (precio, cantidad); break;
            case 1: enviar_NASDAQ(precio, cantidad); break;
            case 2: enviar_BATS  (precio, cantidad); break;
            case 3: enviar_IEX   (precio, cantidad); break;
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    return {"switch 4 casos (aleatorio)", 
            std::chrono::duration<double,std::milli>(t1-t0).count()};
}

// ---- B: if/else binario (condicion aleatoria) ----
ResultadoBench bench_ifelse_aleatorio() {
    resultado_global = 0;
    auto conds = generar_condiciones(2);

    auto t0 = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < N_ORDENES; i++) {
        double precio   = 150.0 + (i % 10);
        int    cantidad = 100   + (i % 50);

        if (conds[i] == 0)   // 50% de las veces el CPU se equivoca
            enviar_NYSE  (precio, cantidad);
        else
            enviar_NASDAQ(precio, cantidad);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    return {"if/else binario (aleatorio, 50% mispred.)",
            std::chrono::duration<double,std::milli>(t1-t0).count()};
}

// ---- C: if/else binario (siempre true -- caso ideal para el CPU) ----
ResultadoBench bench_ifelse_predecible() {
    resultado_global = 0;

    auto t0 = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < N_ORDENES; i++) {
        double precio   = 150.0 + (i % 10);
        int    cantidad = 100   + (i % 50);

        if (true)    // el CPU sabe siempre el resultado -> 0 mispredictions
            enviar_NYSE(precio, cantidad);
        else
            enviar_NASDAQ(precio, cantidad);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    return {"if/else binario (predecible, 0% mispred.)",
            std::chrono::duration<double,std::milli>(t1-t0).count()};
}

// ---- D: Semi-static BINARIO (set_direction cada 1000 ordenes) ----
ResultadoBench bench_semi_static_2() {
    resultado_global = 0;
    auto conds = generar_condiciones(2);

    // Crear BranchChanger con 2 funciones (equivale a if/else)
    BranchChanger<void, double, int> router(enviar_NYSE, enviar_NASDAQ);

    auto t0 = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < N_ORDENES; i++) {
        // COLD PATH: solo 1 vez cada 1000 ordenes
        if (i % 1000 == 0)
            router.set_direction(conds[i]);

        // HOT PATH: jmp directo, sin condicion
        double precio   = 150.0 + (i % 10);
        int    cantidad = 100   + (i % 50);
        router.branch(precio, cantidad);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    return {"Semi-static 2 ramas (set cada 1000 ord.)",
            std::chrono::duration<double,std::milli>(t1-t0).count()};
}

// ---- E: Semi-static GENERALIZADO 4 ramas (set cada 1000 ordenes) ----
ResultadoBench bench_semi_static_4() {
    resultado_global = 0;
    auto conds = generar_condiciones(4);

    // Crear BranchChanger con 4 funciones (equivale a switch de 4 casos)
    BranchChanger<void, double, int> router(
        enviar_NYSE, enviar_NASDAQ, enviar_BATS, enviar_IEX
    );

    auto t0 = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < N_ORDENES; i++) {
        // COLD PATH: solo 1 vez cada 1000 ordenes
        if (i % 1000 == 0)
            router.set_direction(conds[i]);

        // HOT PATH: jmp directo a cualquiera de los 4 exchanges
        double precio   = 150.0 + (i % 10);
        int    cantidad = 100   + (i % 50);
        router.branch(precio, cantidad);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    return {"Semi-static 4 ramas (set cada 1000 ord.)",
            std::chrono::duration<double,std::milli>(t1-t0).count()};
}


// ============================================================
//  PARTE 4: DEMO INTERACTIVA (para entender el flujo)
// ============================================================

void demo_interactiva() {
    std::cout << "\n--- DEMO: como funciona el BranchChanger generalizado ---\n\n";

    // Crear router con 4 exchanges
    BranchChanger<void, double, int> router(
        enviar_NYSE, enviar_NASDAQ, enviar_BATS, enviar_IEX
    );

    struct EstadoMercado {
        int    exchange_optimo; // 0-3
        double volumen_A, volumen_B, volumen_C, volumen_D;
    };

    std::vector<EstadoMercado> escenarios = {
        {0, 9500, 4000, 2000, 1500},   // NYSE tiene mas volumen
        {1, 1000, 8500, 3000, 2000},   // NASDAQ tiene mas volumen
        {3, 2000, 3000, 1500, 9000},   // IEX tiene mas volumen
    };

    std::cout << "  5 millones de ordenes procesadas. Mercado cambia 3 veces.\n\n";

    for (int esc = 0; esc < (int)escenarios.size(); esc++) {
        auto& e = escenarios[esc];
        
        std::cout << "  [Cold Path] Escenario " << (esc+1) << ": "
                  << "Volumenes => NYSE:" << e.volumen_A
                  << " NASDAQ:" << e.volumen_B
                  << " BATS:" << e.volumen_C
                  << " IEX:" << e.volumen_D << "\n";

        // COLD PATH: set_direction con el exchange optimo
        router.set_direction(e.exchange_optimo);
        std::cout << "  [Cold Path] => router configurado a: "
                  << nombres_exchanges[e.exchange_optimo] << "\n";

        // HOT PATH: simular 5 ordenes en este escenario
        std::cout << "  [Hot Path ] 5 ordenes de muestra:\n";
        for (int i = 1; i <= 5; i++) {
            double precio = 100.0 + i * 10.5;
            int cantidad  = i * 100;
            std::cout << "              Orden #" << i
                      << " $" << std::fixed << std::setprecision(1) << precio
                      << " x" << cantidad
                      << " => " << nombres_exchanges[e.exchange_optimo] << "\n";
            router.branch(precio, cantidad);
        }
        std::cout << "\n";
    }
}


// ============================================================
//  PARTE 5: TABLA Y EXPLICACION FINAL
// ============================================================

void imprimir_tabla(const std::vector<ResultadoBench>& r) {
    double base = r[2].tiempo_ms;  // baseline = predecible

    std::cout << "\n";
    std::cout << "  +--------------------------------------------------+----------+----------+\n";
    std::cout << "  | Enfoque                                          | Tiempo   | vs base  |\n";
    std::cout << "  +--------------------------------------------------+----------+----------+\n";

    for (const auto& b : r) {
        double ratio = b.tiempo_ms / base;
        std::cout << "  | " << std::left  << std::setw(48) << b.nombre << " | "
                  << std::right << std::setw(6) << std::fixed << std::setprecision(1)
                  << b.tiempo_ms << " ms | "
                  << std::setw(5) << std::setprecision(2) << ratio << "x  |\n";
    }

    std::cout << "  +--------------------------------------------------+----------+----------+\n";
    std::cout << "  Baseline = if/else predecible (caso ideal, 0 mispredictions)\n";
}

void imprimir_explicacion_cpu() {
    std::cout << "\n";
    std::cout << "  POR QUE EL CPU FALLA AL PREDECIR (branch misprediction)\n";
    std::cout << "  ==========================================================\n\n";
    std::cout << "  El CPU ejecuta instrucciones en pipeline (en paralelo):\n\n";
    std::cout << "  Ciclo:   1     2     3     4     5     6     7\n";
    std::cout << "  Fetch:  [A]   [B]   [C]  [if?]  [D]   [E]   [F]\n";
    std::cout << "  Decode:        [A]   [B]   [C]  [if?]  [D]   [E]\n";
    std::cout << "  Exec:                [A]   [B]   [C]  [if?]  [D]\n\n";
    std::cout << "  Cuando ejecuta [if?] ya ha adelantado D, E, F...\n";
    std::cout << "  Si se equivoca => descarta D, E, F => 13-18 ciclos perdidos\n\n";
    std::cout << "  Con semi-static no hay [if?], solo jmp a direccion conocida.\n";
    std::cout << "  => El CPU nunca descarta nada. 0 ciclos perdidos en hot path.\n";
}

void imprimir_cuando_usar() {
    std::cout << "\n";
    std::cout << "  CUANDO USAR CADA ENFOQUE\n";
    std::cout << "  ========================\n\n";
    std::cout << "  Condicion siempre igual (log level, flag estatico):\n";
    std::cout << "    => if/else normal, el CPU la predice perfectamente\n\n";
    std::cout << "  Condicion aleatoria o muy variable (datos de mercado):\n";
    std::cout << "    => Semi-static conditions (este ejemplo)\n";
    std::cout << "    => La condicion se evalua en cold path, hot path sin checks\n\n";
    std::cout << "  Regla CRITICA de semi-static:\n";
    std::cout << "    OK : set_direction(...) lejos del loop HOT PATH\n";
    std::cout << "    MAL: set_direction + branch en el mismo loop cerrado\n";
    std::cout << "         => SMC machine clears => 30-40x mas lento!\n";
}


// ============================================================
//  MAIN
// ============================================================

int main() {
    std::cout << "\n";
    std::cout << "  ================================================================\n";
    std::cout << "  SEMI-STATIC CONDITIONS vs. IF/ELSE  --  Benchmark HFT\n";
    std::cout << "  Basado en: Bilokon, Lucuta & Shermer (JPDC 2025)\n";
    std::cout << "  ================================================================\n";

    // 1. Demo interactiva del flujo
    demo_interactiva();

    // 2. Benchmarks
    std::cout << "  Ejecutando benchmarks (" << N_ORDENES/1'000'000 << "M ordenes)...\n";
    resultado_global = 0;
    auto r1 = bench_switch_aleatorio();
    std::cout << "  [1/5] switch 4 casos aleatorio:      " << r1.tiempo_ms << " ms\n";

    resultado_global = 0;
    auto r2 = bench_ifelse_aleatorio();
    std::cout << "  [2/5] if/else binario aleatorio:     " << r2.tiempo_ms << " ms\n";

    resultado_global = 0;
    auto r3 = bench_ifelse_predecible();
    std::cout << "  [3/5] if/else binario predecible:    " << r3.tiempo_ms << " ms\n";

    resultado_global = 0;
    auto r4 = bench_semi_static_2();
    std::cout << "  [4/5] semi-static 2 ramas:           " << r4.tiempo_ms << " ms\n";

    resultado_global = 0;
    auto r5 = bench_semi_static_4();
    std::cout << "  [5/5] semi-static 4 ramas:           " << r5.tiempo_ms << " ms\n";

    // 3. Tabla comparativa
    imprimir_tabla({r1, r2, r3, r4, r5});

    // 4. Explicacion del pipeline
    imprimir_explicacion_cpu();

    // 5. Guia de uso
    imprimir_cuando_usar();

    std::cout << "\n  ================================================================\n\n";
    return 0;
}
