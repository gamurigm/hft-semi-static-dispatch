/**
 * ============================================================
 *  TEST SUITE para semi_static.hpp v3.0
 *
 *  Valida las 4 clases + 4 factories en todos los escenarios:
 *    - Construccion, add(), replace()
 *    - set() por indice, por bool
 *    - branch() y operator()
 *    - Retorno de valor (int, double, string)
 *    - Retorno void
 *    - Funciones libres, lambdas, std::bind, functores
 *    - Introspeccion: size(), capacity(), active(), empty()
 *    - Multi-thread con AtomicFastBranch y AtomicFlexBranch
 *    - Benchmark comparativo final
 *
 *  Compilar:
 *    g++ -std=c++17 -O3 -o test_ss.exe test_semi_static.cpp
 *    .\test_ss.exe
 * ============================================================
 */

#include "semi_static.hpp"

#include <iostream>
#include <string>
#include <chrono>
#include <vector>
#include <random>
#include <thread>
#include <iomanip>
#include <cassert>
#include <functional>
#include <sstream>

using namespace semistatic;

// ============================================================
//  Test helpers
// ============================================================

int tests_total   = 0;
int tests_passed  = 0;
int tests_failed  = 0;

#define TEST(name) \
    do { tests_total++; std::cout << "  [" << tests_total << "] " << name << "... "; } while(0)

#define PASS() \
    do { tests_passed++; std::cout << "OK\n"; } while(0)

#define FAIL(msg) \
    do { tests_failed++; std::cout << "FAIL: " << msg << "\n"; } while(0)

#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while(0)

#define CHECK_EQ(a, b, msg) \
    do { if ((a) != (b)) { FAIL(msg << " (got " << (a) << ", expected " << (b) << ")"); return; } } while(0)


// ============================================================
//  Funciones de prueba (libre / static)
// ============================================================

int    fn_add1(int x)    { return x + 1; }
int    fn_add10(int x)   { return x + 10; }
int    fn_add100(int x)  { return x + 100; }
int    fn_mul2(int x)    { return x * 2; }

double fn_half(double x)   { return x / 2.0; }
double fn_double(double x) { return x * 2.0; }
double fn_negate(double x) { return -x; }

void fn_void_a(int& out) { out = 1; }
void fn_void_b(int& out) { out = 2; }
void fn_void_c(int& out) { out = 3; }

volatile double sink_g = 0;
void fn_sink_a(double p, int q) { sink_g += p * q * 1.0002; }
void fn_sink_b(double p, int q) { sink_g += p * q * 0.9998; }
void fn_sink_c(double p, int q) { sink_g += p * q * 1.0000; }
void fn_sink_d(double p, int q) { sink_g += p * q / (q + 1); }

// Functor
struct Multiplier {
    int factor;
    int operator()(int x) const { return x * factor; }
};


// ============================================================
//  TEST 1: FastBranch — Construccion y API basica
// ============================================================

void test_fast_basic() {
    TEST("FastBranch: constructor variadic");
    FastBranch<4, int, int> fb(fn_add1, fn_add10, fn_add100);
    CHECK_EQ(fb.size(), 3, "size");
    CHECK_EQ(fb.capacity(), 4, "capacity");
    CHECK_EQ(fb.active(), 0, "initial active");
    CHECK(!fb.empty(), "not empty");
    PASS();

    TEST("FastBranch: branch() ejecuta rama 0 por defecto");
    int r = fb.branch(5);
    CHECK_EQ(r, 6, "fn_add1(5)");
    PASS();

    TEST("FastBranch: set(1) + operator()");
    fb.set(1);
    CHECK_EQ(fb.active(), 1, "active");
    CHECK_EQ(fb(5), 15, "fn_add10(5)");
    PASS();

    TEST("FastBranch: set(2)");
    fb.set(2);
    CHECK_EQ(fb(5), 105, "fn_add100(5)");
    PASS();

    TEST("FastBranch: set(bool) — true->0, false->1");
    fb.set(true);
    CHECK_EQ(fb.active(), 0, "true->0");
    CHECK_EQ(fb(5), 6, "rama 0");
    fb.set(false);
    CHECK_EQ(fb.active(), 1, "false->1");
    CHECK_EQ(fb(5), 15, "rama 1");
    PASS();

    TEST("FastBranch: set_direction() alias");
    fb.set_direction(2);
    CHECK_EQ(fb.active(), 2, "set_direction");
    fb.select(0);
    CHECK_EQ(fb.active(), 0, "select");
    PASS();
}

void test_fast_add_replace() {
    TEST("FastBranch: default constructor + add()");
    FastBranch<4, int, int> fb;
    CHECK(fb.empty(), "starts empty");
    CHECK_EQ(fb.size(), 0, "size 0");
    int idx0 = fb.add(fn_add1);
    int idx1 = fb.add(fn_add10);
    CHECK_EQ(idx0, 0, "first add returns 0");
    CHECK_EQ(idx1, 1, "second add returns 1");
    CHECK_EQ(fb.size(), 2, "size after add");
    fb.set(0);
    CHECK_EQ(fb(7), 8, "add1(7)");
    fb.set(1);
    CHECK_EQ(fb(7), 17, "add10(7)");
    PASS();

    TEST("FastBranch: replace()");
    auto old = fb.replace(0, fn_mul2);
    CHECK_EQ(old, (FastBranch<4,int,int>::FuncPtr)fn_add1, "returns old ptr");
    fb.set(0);
    CHECK_EQ(fb(7), 14, "mul2(7) after replace");
    PASS();

    TEST("FastBranch: get()");
    CHECK_EQ(fb.get(0), (FastBranch<4,int,int>::FuncPtr)fn_mul2, "get(0)");
    CHECK_EQ(fb.get(1), (FastBranch<4,int,int>::FuncPtr)fn_add10, "get(1)");
    PASS();
}

void test_fast_double_ret() {
    TEST("FastBranch: retorno double");
    FastBranch<3, double, double> fb(fn_half, fn_double, fn_negate);
    fb.set(0);
    CHECK_EQ(fb(10.0), 5.0, "half(10)");
    fb.set(1);
    CHECK_EQ(fb(10.0), 20.0, "double(10)");
    fb.set(2);
    CHECK_EQ(fb(10.0), -10.0, "negate(10)");
    PASS();
}

void test_fast_void() {
    TEST("FastBranch: retorno void con referencia");
    FastBranch<3, void, int&> fb(fn_void_a, fn_void_b, fn_void_c);
    int out = 0;
    fb.set(0); fb(out); CHECK_EQ(out, 1, "void a");
    fb.set(1); fb(out); CHECK_EQ(out, 2, "void b");
    fb.set(2); fb(out); CHECK_EQ(out, 3, "void c");
    PASS();
}

void test_fast_factory() {
    TEST("make_fast: deduccion automatica de firma");
    auto fb = make_fast(fn_add1, fn_add10, fn_add100);
    CHECK_EQ(fb.size(), 3, "size");
    fb.set(2);
    CHECK_EQ(fb(0), 100, "fn_add100(0)");
    PASS();

    TEST("make_fast: 2 ramas double");
    auto fb2 = make_fast(fn_half, fn_double);
    fb2.set(1);
    CHECK_EQ(fb2(5.0), 10.0, "double(5)");
    PASS();
}


// ============================================================
//  TEST 2: FlexBranch — Lambdas, capturas, std::bind
// ============================================================

void test_flex_lambda() {
    TEST("FlexBranch: lambdas sin captura");
    auto fb = make_flex<int(int)>(
        [](int x) { return x + 1; },
        [](int x) { return x + 10; },
        [](int x) { return x * 3; }
    );
    CHECK_EQ(fb.size(), 3, "size");
    fb.set(0);
    CHECK_EQ(fb(5), 6, "lambda +1");
    fb.set(2);
    CHECK_EQ(fb(5), 15, "lambda *3");
    PASS();
}

void test_flex_capture() {
    TEST("FlexBranch: lambdas CON captura");
    int counter_a = 0, counter_b = 0;
    auto fb = make_flex<void(int)>(
        [&counter_a](int x) { counter_a += x; },
        [&counter_b](int x) { counter_b += x; }
    );
    fb.set(0);
    fb(10); fb(20);
    CHECK_EQ(counter_a, 30, "counter_a");
    CHECK_EQ(counter_b, 0, "counter_b untouched");
    fb.set(1);
    fb(5);
    CHECK_EQ(counter_b, 5, "counter_b after set(1)");
    PASS();
}

void test_flex_bind() {
    TEST("FlexBranch: std::bind con metodo de clase");

    struct Calculator {
        int base;
        int compute(int x) const { return base + x; }
    };

    Calculator calc10{10};
    Calculator calc100{100};

    auto fb = make_flex<int(int)>(
        std::bind(&Calculator::compute, &calc10, std::placeholders::_1),
        std::bind(&Calculator::compute, &calc100, std::placeholders::_1)
    );
    fb.set(0);
    CHECK_EQ(fb(5), 15, "calc10.compute(5)");
    fb.set(1);
    CHECK_EQ(fb(5), 105, "calc100.compute(5)");
    PASS();
}

void test_flex_functor() {
    TEST("FlexBranch: functores (struct con operator())");
    auto fb = make_flex<int(int)>(
        Multiplier{2},
        Multiplier{5},
        Multiplier{10}
    );
    fb.set(0); CHECK_EQ(fb(3), 6, "x2");
    fb.set(1); CHECK_EQ(fb(3), 15, "x5");
    fb.set(2); CHECK_EQ(fb(3), 30, "x10");
    PASS();
}

void test_flex_string_return() {
    TEST("FlexBranch: retorno std::string");
    auto fb = make_flex<std::string(int)>(
        [](int code) -> std::string { return "OK-" + std::to_string(code); },
        [](int code) -> std::string { return "ERR-" + std::to_string(code); }
    );
    fb.set(0);
    CHECK_EQ(fb(200), std::string("OK-200"), "ok");
    fb.set(1);
    CHECK_EQ(fb(500), std::string("ERR-500"), "err");
    PASS();
}

void test_flex_add_replace() {
    TEST("FlexBranch: add() en runtime");
    FlexBranch<int(int)> fb(fn_add1, fn_add10);
    CHECK_EQ(fb.size(), 2, "initial");
    int idx = fb.add(fn_mul2);
    CHECK_EQ(idx, 2, "add returns 2");
    CHECK_EQ(fb.size(), 3, "after add");
    fb.set(2);
    CHECK_EQ(fb(6), 12, "mul2(6)");
    PASS();

    TEST("FlexBranch: replace()");
    fb.replace(0, fn_add100);
    fb.set(0);
    CHECK_EQ(fb(1), 101, "replaced with add100");
    PASS();
}

void test_flex_vector_ctor() {
    TEST("FlexBranch: constructor desde vector");
    using Fn = std::function<int(int)>;
    std::vector<Fn> ramas = { fn_add1, fn_add10, fn_add100, fn_mul2 };
    FlexBranch<int(int)> fb(ramas);
    CHECK_EQ(fb.size(), 4, "size from vector");
    fb.set(3);
    CHECK_EQ(fb(5), 10, "mul2(5)");
    PASS();
}


// ============================================================
//  TEST 3: AtomicFastBranch — Multi-thread
// ============================================================

void test_atomic_fast_basic() {
    TEST("AtomicFastBranch: constructor + set + branch");
    AtomicFastBranch<3, int, int> ab(fn_add1, fn_add10, fn_add100);
    CHECK_EQ(ab.size(), 3, "size");
    CHECK_EQ(ab.active(), 0, "initial");
    CHECK_EQ(ab(5), 6, "rama 0");
    ab.set(2);
    CHECK_EQ(ab(5), 105, "rama 2");
    PASS();

    TEST("AtomicFastBranch: set(bool)");
    ab.set(false);
    CHECK_EQ(ab.active(), 1, "false->1");
    CHECK_EQ(ab(5), 15, "rama 1");
    PASS();

    TEST("AtomicFastBranch: replace()");
    ab.replace(0, fn_mul2);
    ab.set(0);
    CHECK_EQ(ab(7), 14, "mul2 after replace");
    PASS();
}

void test_atomic_fast_mt() {
    TEST("AtomicFastBranch: cold+hot threads concurrentes");

    std::atomic<long long> total{0};

    auto fn_inc1 = [](long long* t) { (*t)++; };
    auto fn_inc2 = [](long long* t) { (*t) += 2; };

    // Usamos funciones que escriben a un puntero pasado como argumento
    // pero AtomicFastBranch solo acepta punteros a funcion libre.
    // Usemos funciones void con volatile global en su lugar:
    AtomicFastBranch<4, void, double, int> ab(fn_sink_a, fn_sink_b, fn_sink_c, fn_sink_d);

    std::atomic<bool> running{true};
    const int N_HOT_OPS = 500'000;

    // Cold thread: cambia la direccion cada 1ms
    std::thread cold([&]() {
        int dir = 0;
        while (running.load(std::memory_order_relaxed)) {
            ab.set(dir % 4);
            dir++;
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    });

    // 2 hot threads
    std::thread hot1([&]() {
        for (int i = 0; i < N_HOT_OPS; i++)
            ab(150.0 + (i%10), 100 + (i%50));
    });
    std::thread hot2([&]() {
        for (int i = 0; i < N_HOT_OPS; i++)
            ab(150.0 + (i%10), 100 + (i%50));
    });

    hot1.join();
    hot2.join();
    running.store(false);
    cold.join();

    // Si llegamos aqui sin crash ni hang -> paso
    PASS();
}

void test_atomic_fast_factory() {
    TEST("make_atomic_fast: deduccion automatica");
    auto ab = make_atomic_fast(fn_add1, fn_add10);
    ab.set(1);
    CHECK_EQ(ab(3), 13, "fn_add10(3)");
    PASS();
}


// ============================================================
//  TEST 4: AtomicFlexBranch — Multi-thread con lambdas
// ============================================================

void test_atomic_flex_basic() {
    TEST("AtomicFlexBranch: lambdas con captura");
    int counter = 0;
    auto ab = make_atomic_flex<void(int)>(
        [&counter](int x) { counter += x; },
        [&counter](int x) { counter -= x; }
    );
    ab.set(0); ab(10);
    CHECK_EQ(counter, 10, "+10");
    ab.set(1); ab(3);
    CHECK_EQ(counter, 7, "-3");
    PASS();
}

void test_atomic_flex_mt() {
    TEST("AtomicFlexBranch: multi-thread con lambdas");
    std::atomic<int> total{0};

    auto ab = make_atomic_flex<void()>(
        [&total]() { total.fetch_add(1, std::memory_order_relaxed); },
        [&total]() { total.fetch_add(2, std::memory_order_relaxed); }
    );

    ab.set(0);
    const int N = 100'000;

    std::thread t1([&]() { for (int i = 0; i < N; i++) ab(); });
    std::thread t2([&]() { for (int i = 0; i < N; i++) ab(); });
    t1.join();
    t2.join();

    // Con set(0), cada thread agrega 1 por iteracion -> total >= 2*N
    // (cold puede cambiar direccion, pero no aqui, asi que = 2*N)
    CHECK_EQ(total.load(), 2 * N, "2*N incrementos");
    PASS();
}


// ============================================================
//  TEST 5: Benchmark final
// ============================================================

using TP = std::chrono::high_resolution_clock::time_point;
double elapsed_ms(TP t0) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::high_resolution_clock::now() - t0).count();
}

void test_benchmark() {
    const int N = 5'000'000;

    std::cout << "\n  --- BENCHMARK (" << N/1'000'000 << "M ops) ---\n\n";

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, 3);
    std::vector<int> conds(N);
    for (auto& c : conds) c = dist(rng);

    // A) switch aleatorio
    sink_g = 0;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < N; i++) {
        double p = 150.0 + (i%10); int q = 100 + (i%50);
        switch(conds[i]) {
            case 0: fn_sink_a(p,q); break; case 1: fn_sink_b(p,q); break;
            case 2: fn_sink_c(p,q); break; case 3: fn_sink_d(p,q); break;
        }
    }
    double ms_sw = elapsed_ms(t0);

    // B) FastBranch (set/1000)
    sink_g = 0;
    auto fb = make_fast(fn_sink_a, fn_sink_b, fn_sink_c, fn_sink_d);
    t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < N; i++) {
        if (i % 1000 == 0) fb.set(conds[i]);
        fb(150.0 + (i%10), 100 + (i%50));
    }
    double ms_fb = elapsed_ms(t0);

    // C) FlexBranch (set/1000)
    sink_g = 0;
    auto fl = make_flex<void(double,int)>(fn_sink_a, fn_sink_b, fn_sink_c, fn_sink_d);
    t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < N; i++) {
        if (i % 1000 == 0) fl.set(conds[i]);
        fl(150.0 + (i%10), 100 + (i%50));
    }
    double ms_fl = elapsed_ms(t0);

    // D) Directo (0 branches)
    sink_g = 0;
    t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < N; i++)
        fn_sink_a(150.0 + (i%10), 100 + (i%50));
    double ms_dir = elapsed_ms(t0);

    auto row = [&](const char* name, double ms_v) {
        double sp = ms_sw / ms_v;
        std::cout << "  " << std::left << std::setw(38) << name
                  << std::right << std::fixed
                  << std::setw(8) << std::setprecision(1) << ms_v << " ms"
                  << std::setw(8) << std::setprecision(2) << (ms_v * 1e6 / N) << " ns/op"
                  << std::setw(8) << std::setprecision(2) << sp << "x\n";
    };

    std::cout << "  " << std::left << std::setw(38) << "Enfoque"
              << std::right << std::setw(8) << "Total"
              << std::setw(11) << "Latencia"
              << std::setw(10) << "Speedup" << "\n";
    std::cout << "  " << std::string(67, '-') << "\n";
    row("switch aleatorio (4 ramas)", ms_sw);
    row("FastBranch (set/1000)", ms_fb);
    row("FlexBranch (set/1000)", ms_fl);
    row("Directo (0 branches, ideal)", ms_dir);
    std::cout << "\n";
}


// ============================================================
//  MAIN
// ============================================================

int main() {
    std::cout << "\n";
    std::cout << "  ================================================================\n";
    std::cout << "  TEST SUITE — semi_static.hpp v3.0\n";
    std::cout << "  ================================================================\n\n";

    // FastBranch
    std::cout << "  -- FastBranch --\n";
    test_fast_basic();
    test_fast_add_replace();
    test_fast_double_ret();
    test_fast_void();
    test_fast_factory();

    // FlexBranch
    std::cout << "\n  -- FlexBranch --\n";
    test_flex_lambda();
    test_flex_capture();
    test_flex_bind();
    test_flex_functor();
    test_flex_string_return();
    test_flex_add_replace();
    test_flex_vector_ctor();

    // AtomicFastBranch
    std::cout << "\n  -- AtomicFastBranch --\n";
    test_atomic_fast_basic();
    test_atomic_fast_mt();
    test_atomic_fast_factory();

    // AtomicFlexBranch
    std::cout << "\n  -- AtomicFlexBranch --\n";
    test_atomic_flex_basic();
    test_atomic_flex_mt();

    // Benchmark
    test_benchmark();

    // Resumen
    std::cout << "  ================================================================\n";
    std::cout << "  RESULTADO: " << tests_passed << "/" << tests_total << " tests pasaron";
    if (tests_failed > 0)
        std::cout << " (" << tests_failed << " FALLARON)";
    else
        std::cout << " (todos OK)";
    std::cout << "\n  ================================================================\n\n";

    return tests_failed > 0 ? 1 : 0;
}
