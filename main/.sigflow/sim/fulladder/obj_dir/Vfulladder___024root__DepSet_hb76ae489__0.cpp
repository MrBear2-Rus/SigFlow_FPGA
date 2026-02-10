// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Vfulladder.h for the primary calling header

#include "Vfulladder__pch.h"
#include "Vfulladder___024root.h"

void Vfulladder___024root___ico_sequent__TOP__0(Vfulladder___024root* vlSelf);

void Vfulladder___024root___eval_ico(Vfulladder___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vfulladder___024root___eval_ico\n"); );
    Vfulladder__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    if ((1ULL & vlSelfRef.__VicoTriggered.word(0U))) {
        Vfulladder___024root___ico_sequent__TOP__0(vlSelf);
    }
}

VL_INLINE_OPT void Vfulladder___024root___ico_sequent__TOP__0(Vfulladder___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vfulladder___024root___ico_sequent__TOP__0\n"); );
    Vfulladder__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    vlSelfRef.sum = ((IData)(vlSelfRef.a) ^ ((IData)(vlSelfRef.b) 
                                             ^ (IData)(vlSelfRef.cin)));
    vlSelfRef.cout = (((IData)(vlSelfRef.a) & (IData)(vlSelfRef.b)) 
                      | ((IData)(vlSelfRef.cin) & ((IData)(vlSelfRef.a) 
                                                   | (IData)(vlSelfRef.b))));
}

void Vfulladder___024root___eval_triggers__ico(Vfulladder___024root* vlSelf);

bool Vfulladder___024root___eval_phase__ico(Vfulladder___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vfulladder___024root___eval_phase__ico\n"); );
    Vfulladder__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Init
    CData/*0:0*/ __VicoExecute;
    // Body
    Vfulladder___024root___eval_triggers__ico(vlSelf);
    __VicoExecute = vlSelfRef.__VicoTriggered.any();
    if (__VicoExecute) {
        Vfulladder___024root___eval_ico(vlSelf);
    }
    return (__VicoExecute);
}

void Vfulladder___024root___eval_act(Vfulladder___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vfulladder___024root___eval_act\n"); );
    Vfulladder__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

void Vfulladder___024root___eval_nba(Vfulladder___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vfulladder___024root___eval_nba\n"); );
    Vfulladder__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

void Vfulladder___024root___eval_triggers__act(Vfulladder___024root* vlSelf);

bool Vfulladder___024root___eval_phase__act(Vfulladder___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vfulladder___024root___eval_phase__act\n"); );
    Vfulladder__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Init
    VlTriggerVec<0> __VpreTriggered;
    CData/*0:0*/ __VactExecute;
    // Body
    Vfulladder___024root___eval_triggers__act(vlSelf);
    __VactExecute = vlSelfRef.__VactTriggered.any();
    if (__VactExecute) {
        __VpreTriggered.andNot(vlSelfRef.__VactTriggered, vlSelfRef.__VnbaTriggered);
        vlSelfRef.__VnbaTriggered.thisOr(vlSelfRef.__VactTriggered);
        Vfulladder___024root___eval_act(vlSelf);
    }
    return (__VactExecute);
}

bool Vfulladder___024root___eval_phase__nba(Vfulladder___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vfulladder___024root___eval_phase__nba\n"); );
    Vfulladder__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Init
    CData/*0:0*/ __VnbaExecute;
    // Body
    __VnbaExecute = vlSelfRef.__VnbaTriggered.any();
    if (__VnbaExecute) {
        Vfulladder___024root___eval_nba(vlSelf);
        vlSelfRef.__VnbaTriggered.clear();
    }
    return (__VnbaExecute);
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Vfulladder___024root___dump_triggers__ico(Vfulladder___024root* vlSelf);
#endif  // VL_DEBUG
#ifdef VL_DEBUG
VL_ATTR_COLD void Vfulladder___024root___dump_triggers__nba(Vfulladder___024root* vlSelf);
#endif  // VL_DEBUG
#ifdef VL_DEBUG
VL_ATTR_COLD void Vfulladder___024root___dump_triggers__act(Vfulladder___024root* vlSelf);
#endif  // VL_DEBUG

void Vfulladder___024root___eval(Vfulladder___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vfulladder___024root___eval\n"); );
    Vfulladder__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Init
    IData/*31:0*/ __VicoIterCount;
    CData/*0:0*/ __VicoContinue;
    IData/*31:0*/ __VnbaIterCount;
    CData/*0:0*/ __VnbaContinue;
    // Body
    __VicoIterCount = 0U;
    vlSelfRef.__VicoFirstIteration = 1U;
    __VicoContinue = 1U;
    while (__VicoContinue) {
        if (VL_UNLIKELY(((0x64U < __VicoIterCount)))) {
#ifdef VL_DEBUG
            Vfulladder___024root___dump_triggers__ico(vlSelf);
#endif
            VL_FATAL_MT("D:\\MywxProject\\SigFlow\\Simulation\\fulladder.v", 4, "", "Input combinational region did not converge.");
        }
        __VicoIterCount = ((IData)(1U) + __VicoIterCount);
        __VicoContinue = 0U;
        if (Vfulladder___024root___eval_phase__ico(vlSelf)) {
            __VicoContinue = 1U;
        }
        vlSelfRef.__VicoFirstIteration = 0U;
    }
    __VnbaIterCount = 0U;
    __VnbaContinue = 1U;
    while (__VnbaContinue) {
        if (VL_UNLIKELY(((0x64U < __VnbaIterCount)))) {
#ifdef VL_DEBUG
            Vfulladder___024root___dump_triggers__nba(vlSelf);
#endif
            VL_FATAL_MT("D:\\MywxProject\\SigFlow\\Simulation\\fulladder.v", 4, "", "NBA region did not converge.");
        }
        __VnbaIterCount = ((IData)(1U) + __VnbaIterCount);
        __VnbaContinue = 0U;
        vlSelfRef.__VactIterCount = 0U;
        vlSelfRef.__VactContinue = 1U;
        while (vlSelfRef.__VactContinue) {
            if (VL_UNLIKELY(((0x64U < vlSelfRef.__VactIterCount)))) {
#ifdef VL_DEBUG
                Vfulladder___024root___dump_triggers__act(vlSelf);
#endif
                VL_FATAL_MT("D:\\MywxProject\\SigFlow\\Simulation\\fulladder.v", 4, "", "Active region did not converge.");
            }
            vlSelfRef.__VactIterCount = ((IData)(1U) 
                                         + vlSelfRef.__VactIterCount);
            vlSelfRef.__VactContinue = 0U;
            if (Vfulladder___024root___eval_phase__act(vlSelf)) {
                vlSelfRef.__VactContinue = 1U;
            }
        }
        if (Vfulladder___024root___eval_phase__nba(vlSelf)) {
            __VnbaContinue = 1U;
        }
    }
}

#ifdef VL_DEBUG
void Vfulladder___024root___eval_debug_assertions(Vfulladder___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Vfulladder___024root___eval_debug_assertions\n"); );
    Vfulladder__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    if (VL_UNLIKELY(((vlSelfRef.a & 0xfeU)))) {
        Verilated::overWidthError("a");}
    if (VL_UNLIKELY(((vlSelfRef.b & 0xfeU)))) {
        Verilated::overWidthError("b");}
    if (VL_UNLIKELY(((vlSelfRef.cin & 0xfeU)))) {
        Verilated::overWidthError("cin");}
}
#endif  // VL_DEBUG
