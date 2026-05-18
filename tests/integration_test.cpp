/**
 * @file integration_test.cpp
 * @brief Comprehensive Integration Test for the Semi-Static library.
 *
 * This test integrates all three main variants:
 *  1. FastBranch (Static functions)
 *  2. MemberFastBranch (Instance methods)
 *  3. FlexBranch (Callables / Lambdas)
 *
 * It simulates a complete HFT Gateway pipeline.
 */

#include "../include/semi_static.hpp"
#include <iostream>
#include <cassert>
#include <string>

using namespace semistatic;

// ============================================================================
//  COMPONENTES DE PRUEBA
// ============================================================================

struct Order {
    int id;
    double price;
};

// Componente para MemberFastBranch
class RiskEngine {
public:
    void check_pass(const Order& o) { 
        std::cout << "[Risk] Orden " << o.id << " PASO riesgo.\n"; 
        risk_passed_++;
    }
    void check_fail(const Order& o) { 
        std::cout << "[Risk] Orden " << o.id << " FALLO riesgo.\n"; 
        risk_failed_++;
    }
    
    int risk_passed_ = 0;
    int risk_failed_ = 0;
};

// Componente para FastBranch (Funciones libres)
void process_new_order(const Order& o) {
    std::cout << "[Msg] Procesando NUEVA ORDEN: " << o.id << "\n";
}
void process_cancel(const Order& o) {
    std::cout << "[Msg] Procesando CANCELACION: " << o.id << "\n";
}

// ============================================================================
//  MAIN INTEGRATION TEST
// ============================================================================

int main() {
    std::cout << "================================================================\n";
    std::cout << "  RUNNING FULL INTEGRATION TEST (Fast + Member + Flex) \n";
    std::cout << "================================================================\n\n";

    // 1. Instanciamos los 3 enrutadores
    FastBranch<2, void, const Order&> msg_router(process_new_order, process_cancel);
    MemberFastBranch<2, RiskEngine, void, const Order&> risk_router(&RiskEngine::check_pass, &RiskEngine::check_fail);
    
    // FlexBranch para Logger dinámico
    FlexBranch<void(const std::string&)> log_router(
        [](const std::string& msg) { std::cout << "[LOG:VERBOSE] " << msg << "\n"; },
        [](const std::string&) { /* Quiet mode */ }
    );

    RiskEngine risk_engine;
    Order o1{101, 1500.50};
    Order o2{102, 2500.00};

    // --- ESCENARIO 1: Operacion Normal ---
    std::cout << "--- Escenario 1: Operacion Normal ---\n";
    log_router("Iniciando procesamiento de orden 101");
    
    msg_router.set(0); // Nueva Orden
    risk_router.set(0); // Pasa riesgo
    
    msg_router(o1);
    risk_router(&risk_engine, o1);

    // --- ESCENARIO 2: Mercado Estresado / Bloqueo ---
    std::cout << "\n--- Escenario 2: Mercado Estresado / Bloqueo ---\n";
    log_router.set(1); // Modo silencioso
    log_router("Esto no deberia verse porque el logger esta en modo silencioso");
    
    msg_router.set(1); // Cancelacion
    risk_router.set(1); // Falla riesgo
    
    msg_router(o2);
    risk_router(&risk_engine, o2);

    // --- VERIFICACIONES FINALES (ASSERTS) ---
    std::cout << "\n[SYSTEM] Verificando estado final del sistema...\n";
    assert(risk_engine.risk_passed_ == 1);
    assert(risk_engine.risk_failed_ == 1);
    
    std::cout << "[OK] Integracion exitosa. Todos los componentes cooperaron sin branches.\n";
    std::cout << "================================================================\n";

    return 0;
}
