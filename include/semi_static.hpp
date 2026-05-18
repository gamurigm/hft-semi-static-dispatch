/**
 * @file semi_static.hpp
 * @brief Condiciones Semi-Estaticas para C++17 — Biblioteca Header-Only
 *
 * Implementacion generalizada del patron de eliminacion de branch misprediction
 * descrito en Bilokon, Lucuta & Shermer (JPDC 2025). Proporciona dos variantes
 * optimizadas para distintos escenarios de uso:
 *
 *   - FastBranch<N, Ret, Args...>       Maximo rendimiento, punteros crudos
 *   - FlexBranch<Ret(Args...)>          Flexible, acepta cualquier callable
 *
 * Ambas variantes disponibles en version single-thread y atomic (multi-thread).
 *
 * @version 3.0.0
 * @date 2025
 * @copyright MIT License
 *
 * Requisitos: C++17 o superior.
 *
 * Uso rapido:
 * @code
 *   #include "semi_static.hpp"
 *   using namespace semistatic;
 *
 *   // --- Maximo rendimiento ---
 *   FastBranch<4, void, double, int> router(fn_A, fn_B, fn_C, fn_D);
 *   router.set(2);            // COLD PATH
 *   router(precio, cantidad); // HOT PATH
 *
 *   // --- Flexible (lambdas, capturas) ---
 *   auto logger = make_flex<void(int)>(lambda_debug, lambda_off);
 *   logger.set(0);            // COLD PATH
 *   logger(42);               // HOT PATH
 *
 *   // --- Multi-thread ---
 *   AtomicFastBranch<4, void, Order> router(fn_A, fn_B, fn_C, fn_D);
 *   // Thread A (cold): router.set(idx);
 *   // Thread B (hot):  router(order);  // atomic load, sin mutex
 * @endcode
 *
 * Referencia:
 *   Bilokon, P.A., Lucuta, M., & Shermer, E. (2025). Semi-static conditions
 *   in low-latency C++ for high frequency trading: Better than branch prediction
 *   hints. JPDC, 196, 105000. https://doi.org/10.1016/j.jpdc.2024.105000
 */

#pragma once

#include <functional>
#include <vector>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace semistatic {

// ============================================================================
//  SECCION 1: FastBranch — Maximo Rendimiento (punteros crudos, stack)
//
//  ~3.17 ns/op vs ~8.91 ns/op del switch equivalente (2.81x speedup)
//  Solo acepta funciones libres o static. Sin heap. Sin std::function.
// ============================================================================

template <int MaxN, typename Ret, typename... Args>
class FastBranch {
public:
    using FuncPtr   = Ret(*)(Args...);
    using size_type = int;

    static_assert(MaxN >= 1, "FastBranch requiere MaxN >= 1.");

    // ---- Constructores ----

    /// Variadic: registra funciones en tiempo de compilacion.
    template <typename... Funcs>
    constexpr explicit FastBranch(Funcs... fns) noexcept
        : ptrs_{static_cast<FuncPtr>(fns)...}
        , count_(sizeof...(Funcs))
        , current_(0)
    {
        static_assert(sizeof...(Funcs) >= 1,    "Requiere al menos 1 rama.");
        static_assert(sizeof...(Funcs) <= MaxN, "Excede MaxN ramas.");
    }

    /// Default: sin ramas, requiere add() antes de usar.
    constexpr FastBranch() noexcept : ptrs_{}, count_(0), current_(0) {}

    // ---- Configuracion dinamica ----

    /// Agrega rama en runtime. Retorna indice asignado.
    int add(FuncPtr fn) noexcept {
        assert(fn != nullptr && "FastBranch::add: puntero nulo.");
        assert(count_ < MaxN && "FastBranch::add: capacidad excedida.");
        ptrs_[count_] = fn;
        return count_++;
    }

    /// Reemplaza una rama existente por otra. Retorna la anterior.
    FuncPtr replace(int idx, FuncPtr fn) noexcept {
        assert(idx >= 0 && idx < count_);
        assert(fn != nullptr);
        FuncPtr old = ptrs_[idx];
        ptrs_[idx] = fn;
        return old;
    }

    // ---- COLD PATH ----

    void set(int idx) noexcept {
        assert(idx >= 0 && idx < count_);
        current_ = idx;
    }
    void set(bool cond) noexcept { current_ = cond ? 0 : 1; }

    /// Aliases
    void set_direction(int i) noexcept  { set(i); }
    void set_direction(bool c) noexcept { set(c); }
    void select(int i) noexcept         { set(i); }

    // ---- HOT PATH ----

    Ret branch(Args... args) const {
        return ptrs_[current_](std::forward<Args>(args)...);
    }
    Ret operator()(Args... args) const {
        return ptrs_[current_](std::forward<Args>(args)...);
    }

    // ---- Introspeccion ----

    [[nodiscard]] constexpr int  size()     const noexcept { return count_; }
    [[nodiscard]] constexpr int  capacity() const noexcept { return MaxN; }
    [[nodiscard]] int            active()   const noexcept { return current_; }
    [[nodiscard]] bool           empty()    const noexcept { return count_ == 0; }

    /// Acceso directo al puntero (debug/testing).
    [[nodiscard]] FuncPtr get(int idx) const noexcept {
        assert(idx >= 0 && idx < count_);
        return ptrs_[idx];
    }

private:
    FuncPtr ptrs_[MaxN] = {};
    int     count_   = 0;
    int     current_ = 0;
};


// ============================================================================
//  SECCION 2: AtomicFastBranch — Multi-thread con punteros crudos
//
//  set()  → memory_order_release
//  branch → memory_order_acquire
//  current_ alineado a 64B para evitar false sharing.
// ============================================================================

template <int MaxN, typename Ret, typename... Args>
class AtomicFastBranch {
public:
    using FuncPtr = Ret(*)(Args...);

    static_assert(MaxN >= 1, "AtomicFastBranch requiere MaxN >= 1.");

    template <typename... Funcs>
    explicit AtomicFastBranch(Funcs... fns) noexcept
        : ptrs_{static_cast<FuncPtr>(fns)...}
        , count_(sizeof...(Funcs))
    {
        static_assert(sizeof...(Funcs) >= 1,    "Requiere al menos 1 rama.");
        static_assert(sizeof...(Funcs) <= MaxN, "Excede MaxN ramas.");
        current_.store(0, std::memory_order_relaxed);
    }

    // COLD PATH
    void set(int idx) noexcept {
        assert(idx >= 0 && idx < count_);
        current_.store(idx, std::memory_order_release);
    }
    void set(bool cond) noexcept {
        current_.store(cond ? 0 : 1, std::memory_order_release);
    }
    void set_direction(int i) noexcept  { set(i); }
    void set_direction(bool c) noexcept { set(c); }

    // HOT PATH
    Ret branch(Args... args) const {
        int idx = current_.load(std::memory_order_acquire);
        return ptrs_[idx](std::forward<Args>(args)...);
    }
    Ret operator()(Args... args) const { return branch(std::forward<Args>(args)...); }

    [[nodiscard]] int size()     const noexcept { return count_; }
    [[nodiscard]] int capacity() const noexcept { return MaxN; }
    [[nodiscard]] int active()   const noexcept {
        return current_.load(std::memory_order_relaxed);
    }

    FuncPtr replace(int idx, FuncPtr fn) noexcept {
        assert(idx >= 0 && idx < count_);
        assert(fn != nullptr);
        FuncPtr old = ptrs_[idx];
        ptrs_[idx] = fn;
        return old;
    }

private:
    FuncPtr             ptrs_[MaxN] = {};
    int                 count_ = 0;
    alignas(64) mutable std::atomic<int> current_{0};
};


// ============================================================================
//  SECCION 3: FlexBranch — Flexible con std::function
//
//  Acepta cualquier callable: lambdas con captura, std::bind, functores.
//  Overhead vs FastBranch: ~15% (type erasure de std::function).
// ============================================================================

template <typename Sig>
class FlexBranch;

template <typename Ret, typename... Args>
class FlexBranch<Ret(Args...)> {
public:
    using Callable = std::function<Ret(Args...)>;

    /// Variadic desde cualquier callable.
    template <typename... Funcs>
    explicit FlexBranch(Funcs&&... fns)
        : branches_{ Callable(std::forward<Funcs>(fns))... }
        , current_(0)
    {
        static_assert(sizeof...(Funcs) >= 1, "Requiere al menos 1 rama.");
    }

    /// Desde vector (ramas en runtime).
    explicit FlexBranch(std::vector<Callable> ramas)
        : branches_(std::move(ramas)), current_(0)
    {
        assert(!branches_.empty());
    }

    /// Agrega rama. Retorna indice.
    int add(Callable fn) {
        branches_.push_back(std::move(fn));
        return (int)branches_.size() - 1;
    }

    /// Reemplaza rama existente.
    Callable replace(int idx, Callable fn) {
        assert(idx >= 0 && idx < (int)branches_.size());
        Callable old = std::move(branches_[idx]);
        branches_[idx] = std::move(fn);
        return old;
    }

    // COLD PATH
    void set(int idx) {
        assert(idx >= 0 && idx < (int)branches_.size());
        current_ = idx;
    }
    void set(bool cond)            { current_ = cond ? 0 : 1; }
    void set_direction(int i)      { set(i); }
    void set_direction(bool c)     { set(c); }
    void select(int i)             { set(i); }

    // HOT PATH
    Ret branch(Args... args) {
        return branches_[current_](std::forward<Args>(args)...);
    }
    Ret operator()(Args... args) {
        return branches_[current_](std::forward<Args>(args)...);
    }

    [[nodiscard]] int  size()   const noexcept { return (int)branches_.size(); }
    [[nodiscard]] int  active() const noexcept { return current_; }
    [[nodiscard]] bool empty()  const noexcept { return branches_.empty(); }

private:
    std::vector<Callable> branches_;
    int                   current_ = 0;
};


// ============================================================================
//  SECCION 4: AtomicFlexBranch — Multi-thread con std::function
// ============================================================================

template <typename Sig>
class AtomicFlexBranch;

template <typename Ret, typename... Args>
class AtomicFlexBranch<Ret(Args...)> {
public:
    using Callable = std::function<Ret(Args...)>;

    template <typename... Funcs>
    explicit AtomicFlexBranch(Funcs&&... fns)
        : branches_{ Callable(std::forward<Funcs>(fns))... }
    {
        static_assert(sizeof...(Funcs) >= 1, "Requiere al menos 1 rama.");
        current_.store(0, std::memory_order_relaxed);
    }

    explicit AtomicFlexBranch(std::vector<Callable> ramas)
        : branches_(std::move(ramas))
    {
        assert(!branches_.empty());
        current_.store(0, std::memory_order_relaxed);
    }

    int add(Callable fn) {
        branches_.push_back(std::move(fn));
        return (int)branches_.size() - 1;
    }

    Callable replace(int idx, Callable fn) {
        assert(idx >= 0 && idx < (int)branches_.size());
        Callable old = std::move(branches_[idx]);
        branches_[idx] = std::move(fn);
        return old;
    }

    // COLD PATH
    void set(int idx) {
        assert(idx >= 0 && idx < (int)branches_.size());
        current_.store(idx, std::memory_order_release);
    }
    void set(bool cond) {
        current_.store(cond ? 0 : 1, std::memory_order_release);
    }
    void set_direction(int i)   { set(i); }
    void set_direction(bool c)  { set(c); }

    // HOT PATH
    Ret branch(Args... args) {
        int idx = current_.load(std::memory_order_acquire);
        return branches_[idx](std::forward<Args>(args)...);
    }
    Ret operator()(Args... args) { return branch(std::forward<Args>(args)...); }

    [[nodiscard]] int  size()   const noexcept { return (int)branches_.size(); }
    [[nodiscard]] int  active() const noexcept {
        return current_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool empty()  const noexcept { return branches_.empty(); }

private:
    std::vector<Callable>        branches_;
    alignas(64) std::atomic<int> current_{0};
};


// ============================================================================
//  SECCION 5: Factory Functions
// ============================================================================

/// make_fast(fn_A, fn_B, fn_C) → FastBranch<3, Ret, Args...>
template <typename Ret, typename... Args, typename... Fs>
[[nodiscard]] auto make_fast(Ret(*first)(Args...), Fs... rest) noexcept {
    constexpr int N = 1 + sizeof...(Fs);
    return FastBranch<N, Ret, Args...>(first, static_cast<Ret(*)(Args...)>(rest)...);
}

/// make_atomic_fast(fn_A, fn_B) → AtomicFastBranch<2, Ret, Args...>
template <typename Ret, typename... Args, typename... Fs>
[[nodiscard]] auto make_atomic_fast(Ret(*first)(Args...), Fs... rest) noexcept {
    constexpr int N = 1 + sizeof...(Fs);
    return AtomicFastBranch<N, Ret, Args...>(first, static_cast<Ret(*)(Args...)>(rest)...);
}

/// make_flex<void(int)>(lambda1, lambda2) → FlexBranch<void(int)>
template <typename Sig, typename... Funcs>
[[nodiscard]] auto make_flex(Funcs&&... fns) {
    return FlexBranch<Sig>(std::forward<Funcs>(fns)...);
}

/// make_atomic_flex<void(int)>(l1, l2) → AtomicFlexBranch<void(int)>
template <typename Sig, typename... Funcs>
[[nodiscard]] auto make_atomic_flex(Funcs&&... fns) {
    return AtomicFlexBranch<Sig>(std::forward<Funcs>(fns)...);
}

} // namespace semistatic
