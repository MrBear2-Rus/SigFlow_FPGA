// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Model implementation (design independent parts)

#include "Vfulladder__pch.h"
#include "verilated_vcd_c.h"

//============================================================
// Constructors

Vfulladder::Vfulladder(VerilatedContext* _vcontextp__, const char* _vcname__)
    : VerilatedModel{*_vcontextp__}
    , vlSymsp{new Vfulladder__Syms(contextp(), _vcname__, this)}
    , a{vlSymsp->TOP.a}
    , b{vlSymsp->TOP.b}
    , cin{vlSymsp->TOP.cin}
    , sum{vlSymsp->TOP.sum}
    , cout{vlSymsp->TOP.cout}
    , rootp{&(vlSymsp->TOP)}
{
    // Register model with the context
    contextp()->addModel(this);
    contextp()->traceBaseModelCbAdd(
        [this](VerilatedTraceBaseC* tfp, int levels, int options) { traceBaseModel(tfp, levels, options); });
}

Vfulladder::Vfulladder(const char* _vcname__)
    : Vfulladder(Verilated::threadContextp(), _vcname__)
{
}

//============================================================
// Destructor

Vfulladder::~Vfulladder() {
    delete vlSymsp;
}

//============================================================
// Evaluation function

#ifdef VL_DEBUG
void Vfulladder___024root___eval_debug_assertions(Vfulladder___024root* vlSelf);
#endif  // VL_DEBUG
void Vfulladder___024root___eval_static(Vfulladder___024root* vlSelf);
void Vfulladder___024root___eval_initial(Vfulladder___024root* vlSelf);
void Vfulladder___024root___eval_settle(Vfulladder___024root* vlSelf);
void Vfulladder___024root___eval(Vfulladder___024root* vlSelf);

void Vfulladder::eval_step() {
    VL_DEBUG_IF(VL_DBG_MSGF("+++++TOP Evaluate Vfulladder::eval_step\n"); );
#ifdef VL_DEBUG
    // Debug assertions
    Vfulladder___024root___eval_debug_assertions(&(vlSymsp->TOP));
#endif  // VL_DEBUG
    vlSymsp->__Vm_activity = true;
    vlSymsp->__Vm_deleter.deleteAll();
    if (VL_UNLIKELY(!vlSymsp->__Vm_didInit)) {
        vlSymsp->__Vm_didInit = true;
        VL_DEBUG_IF(VL_DBG_MSGF("+ Initial\n"););
        Vfulladder___024root___eval_static(&(vlSymsp->TOP));
        Vfulladder___024root___eval_initial(&(vlSymsp->TOP));
        Vfulladder___024root___eval_settle(&(vlSymsp->TOP));
    }
    VL_DEBUG_IF(VL_DBG_MSGF("+ Eval\n"););
    Vfulladder___024root___eval(&(vlSymsp->TOP));
    // Evaluate cleanup
    Verilated::endOfEval(vlSymsp->__Vm_evalMsgQp);
}

//============================================================
// Events and timing
bool Vfulladder::eventsPending() { return false; }

uint64_t Vfulladder::nextTimeSlot() {
    VL_FATAL_MT(__FILE__, __LINE__, "", "No delays in the design");
    return 0;
}

//============================================================
// Utilities

const char* Vfulladder::name() const {
    return vlSymsp->name();
}

//============================================================
// Invoke final blocks

void Vfulladder___024root___eval_final(Vfulladder___024root* vlSelf);

VL_ATTR_COLD void Vfulladder::final() {
    Vfulladder___024root___eval_final(&(vlSymsp->TOP));
}

//============================================================
// Implementations of abstract methods from VerilatedModel

const char* Vfulladder::hierName() const { return vlSymsp->name(); }
const char* Vfulladder::modelName() const { return "Vfulladder"; }
unsigned Vfulladder::threads() const { return 1; }
void Vfulladder::prepareClone() const { contextp()->prepareClone(); }
void Vfulladder::atClone() const {
    contextp()->threadPoolpOnClone();
}
std::unique_ptr<VerilatedTraceConfig> Vfulladder::traceConfig() const {
    return std::unique_ptr<VerilatedTraceConfig>{new VerilatedTraceConfig{false, false, false}};
};

//============================================================
// Trace configuration

void Vfulladder___024root__trace_decl_types(VerilatedVcd* tracep);

void Vfulladder___024root__trace_init_top(Vfulladder___024root* vlSelf, VerilatedVcd* tracep);

VL_ATTR_COLD static void trace_init(void* voidSelf, VerilatedVcd* tracep, uint32_t code) {
    // Callback from tracep->open()
    Vfulladder___024root* const __restrict vlSelf VL_ATTR_UNUSED = static_cast<Vfulladder___024root*>(voidSelf);
    Vfulladder__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    if (!vlSymsp->_vm_contextp__->calcUnusedSigs()) {
        VL_FATAL_MT(__FILE__, __LINE__, __FILE__,
            "Turning on wave traces requires Verilated::traceEverOn(true) call before time 0.");
    }
    vlSymsp->__Vm_baseCode = code;
    tracep->pushPrefix(std::string{vlSymsp->name()}, VerilatedTracePrefixType::SCOPE_MODULE);
    Vfulladder___024root__trace_decl_types(tracep);
    Vfulladder___024root__trace_init_top(vlSelf, tracep);
    tracep->popPrefix();
}

VL_ATTR_COLD void Vfulladder___024root__trace_register(Vfulladder___024root* vlSelf, VerilatedVcd* tracep);

VL_ATTR_COLD void Vfulladder::traceBaseModel(VerilatedTraceBaseC* tfp, int levels, int options) {
    (void)levels; (void)options;
    VerilatedVcdC* const stfp = dynamic_cast<VerilatedVcdC*>(tfp);
    if (VL_UNLIKELY(!stfp)) {
        vl_fatal(__FILE__, __LINE__, __FILE__,"'Vfulladder::trace()' called on non-VerilatedVcdC object;"
            " use --trace-fst with VerilatedFst object, and --trace-vcd with VerilatedVcd object");
    }
    stfp->spTrace()->addModel(this);
    stfp->spTrace()->addInitCb(&trace_init, &(vlSymsp->TOP));
    Vfulladder___024root__trace_register(&(vlSymsp->TOP), stfp->spTrace());
}
