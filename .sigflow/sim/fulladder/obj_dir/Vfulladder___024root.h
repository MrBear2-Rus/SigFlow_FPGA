// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design internal header
// See Vfulladder.h for the primary calling header

#ifndef VERILATED_VFULLADDER___024ROOT_H_
#define VERILATED_VFULLADDER___024ROOT_H_  // guard

#include "verilated.h"


class Vfulladder__Syms;

class alignas(VL_CACHE_LINE_BYTES) Vfulladder___024root final : public VerilatedModule {
  public:

    // DESIGN SPECIFIC STATE
    VL_IN8(a,0,0);
    VL_IN8(b,0,0);
    VL_IN8(cin,0,0);
    VL_OUT8(sum,0,0);
    VL_OUT8(cout,0,0);
    CData/*0:0*/ __VstlFirstIteration;
    CData/*0:0*/ __VicoFirstIteration;
    CData/*0:0*/ __VactContinue;
    IData/*31:0*/ __VactIterCount;
    VlTriggerVec<1> __VstlTriggered;
    VlTriggerVec<1> __VicoTriggered;
    VlTriggerVec<0> __VactTriggered;
    VlTriggerVec<0> __VnbaTriggered;

    // INTERNAL VARIABLES
    Vfulladder__Syms* const vlSymsp;

    // CONSTRUCTORS
    Vfulladder___024root(Vfulladder__Syms* symsp, const char* v__name);
    ~Vfulladder___024root();
    VL_UNCOPYABLE(Vfulladder___024root);

    // INTERNAL METHODS
    void __Vconfigure(bool first);
};


#endif  // guard
