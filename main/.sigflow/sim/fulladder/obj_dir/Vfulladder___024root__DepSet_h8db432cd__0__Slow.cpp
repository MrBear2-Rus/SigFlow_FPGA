// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Vfulladder.h for the primary calling header

#include "Vfulladder__pch.h"
#include "Vfulladder__Syms.h"
#include "Vfulladder___024root.h"

#ifdef VL_DEBUG
VL_ATTR_COLD void Vfulladder___024root___dump_triggers__stl(Vfulladder___024root* vlSelf);
#endif  // VL_DEBUG

VL_ATTR_COLD void Vfulladder___024root___eval_triggers__stl(Vfulladder___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vfulladder___024root___eval_triggers__stl\n"); );
    Vfulladder__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    vlSelfRef.__VstlTriggered.setBit(0U, (IData)(vlSelfRef.__VstlFirstIteration));
#ifdef VL_DEBUG
    if (VL_UNLIKELY(vlSymsp->_vm_contextp__->debug())) {
        Vfulladder___024root___dump_triggers__stl(vlSelf);
    }
#endif
}
