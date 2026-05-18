# 🛠️ Documentación Técnica: Biblioteca Semi-Static

Esta biblioteca implementa el patrón de **Despacho Semi-Estático** (*Semi-Static Dispatch*) descrito por Bilokon et al. (2025) para sistemas de ultra-baja latencia (C++17). Su objetivo principal es eliminar las penalizaciones por fallos de predicción de saltos (*branch mispredictions*) en el cauce del CPU.

La biblioteca es *header-only* y se encuentra en `include/semi_static.hpp`.

---

## 🚀 Las 4 Variantes de Despacho

La biblioteca ofrece cuatro herramientas especializadas según el caso de uso y el nivel de dinamismo requerido:

### 1. `FastBranch<N, Ret, Args...>`
Es la variante de máximo rendimiento. Utiliza un arreglo de punteros a funciones crudas en el Stack. No realiza asignaciones en el Heap ni usa `std::function`.

* **Garantía:** Cero asignaciones.
* **Uso ideal:** Despacho de alta frecuencia en un solo hilo donde las funciones destino son libres o estáticas.
* **Firmas estrictas:** Incluye un `static_assert` para garantizar que todas las funciones coincidan exactamente con la firma sin conversiones implícitas.

```cpp
void action1(int x); void action2(int x);
FastBranch<2, void, int> router(action1, action2);
router.set(1); // Cold Path
router(42);    // Hot Path
```

### 2. `AtomicFastBranch<N, Ret, Args...>`
Versión segura para multi-hilo de `FastBranch`. El índice de la rama activa se almacena en un entero atómico con alineamiento de 64 bytes (`alignas(64)`) para evitar el falso compartimiento de caché (*False Sharing*).

* **Garantía:** Hilo seguro, libre de bloqueos (*lock-free*), usa `memory_order_acquire/release`.
* **Uso ideal:** Sistemas donde un hilo de control (Cold Path) cambia la estrategia de ejecución mientras un hilo de red (Hot Path) procesa millones de mensajes.

```cpp
AtomicFastBranch<2, void, int> router(action1, action2);
// En Hilo A: router.set(1);
// En Hilo B: router(42);
```

### 3. `MemberFastBranch<N, T, Ret, Args...>`
**¡Nueva Característica!** Permite despachar métodos de instancia de una clase sin el costo de `std::function` ni memoria dinámica. 

* **Garantía:** Polimorfismo orientado a objetos con rendimiento cercano al sub-nanosegundo.
* **Uso ideal:** Llamar a métodos específicos de un objeto de negocio (como un Motor de Riesgo o Libro de Órdenes) sin usar tablas virtuales.

```cpp
class Trader { public: void buy(int x); void sell(int x); };
Trader trader;
MemberFastBranch<2, Trader, void, int> router(&Trader::buy, &Trader::sell);
router(&trader, 100); // Se pasa la instancia como primer argumento
```

### 4. `FlexBranch<Ret(Args...)>`
Es la variante más flexible. Utiliza un `std::vector` de `std::function`. Puede crecer dinámicamente en tiempo de ejecución y acepta cualquier objeto invocable (*callable*), incluyendo lambdas con captura y `std::bind`.

* **Costo:** Aproximadamente un 15% de overhead respecto a `FastBranch` debido al *type erasure* de `std::function`.
* **Uso ideal:** Enrutamiento de logs, sistemas de fallback o lógicas complejas donde la flexibilidad supera a la necesidad de nanosegundos puros.

```cpp
FlexBranch<void(int)> router;
router.add([](int x) { std::cout << x; });
router.add([factor](int x) { std::cout << x * factor; });
```

---

## 📊 Pruebas y Benchmarks

La biblioteca incluye una suite completa de pruebas en la carpeta `tests/` y múltiples escenarios avanzados en `examples/`:

* `tests/integration_test.cpp`: Prueba la cooperación de las variantes trabajando juntas en un pipeline.
* `examples/extreme_stress_test.cpp`: Somete al sistema a ráfagas de 10,000 cambios de estrategia por segundo.
* `examples/liquidity_aware_gateway.cpp`: Simula un Gateway que cambia de algoritmo según el modelo matemático de liquidez del mercado.

---

## 💻 Compilación

Para obtener el máximo rendimiento y activar todas las optimizaciones del CPU (como el vectorizado y el alineamiento de caché), se recomienda compilar con la bandera `-O3` y `-march=native`:

```bash
g++ -std=c++17 -O3 -march=native -pthread tu_archivo.cpp -o tu_ejecutable
```
