// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Vfulladder.h for the primary calling header

#include "Vfulladder__pch.h"
#include "Vfulladder__Syms.h"
#include "Vfulladder___024root.h"

void Vfulladder___024root___ctor_var_reset(Vfulladder___024root* vlSelf);

Vfulladder___024root::Vfulladder___024root(Vfulladder__Syms* symsp, const char* v__name)
    : VerilatedModule{v__name}
    , vlSymsp{symsp}
 {
    // Reset structure values
    Vfulladder___024root___ctor_var_reset(this);
}

void Vfulladder___024root::__Vconfigure(bool first) {
    (void)first;  // Prevent unused variable warning
}

Vfulladder___024root::~Vfulladder___024root() {
}
