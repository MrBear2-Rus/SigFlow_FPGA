#pragma once
#include "Phase1Faults.h"
namespace eda::virtual_board {
// Phase 4 teaching faults (planning book §3.1, remaining high-level rows):
//  - i2c_nak: the sensor releases its ACK slot so the master samples WHI.
//  - spi_mode_fault: wrong-edge or bit-slipped sampling of the OLED stream.
//  - crc_tamper: flip the CRC byte of one upper-layer packet while every
//    bottom-layer UART 8N1 bit stays fully legal (explicit protocol_id).
//  - cdc_sampling_approx: functional metastability approximation; a fixed
//    pcg32-v1 stream deterministically enumerates the sampling delay branch
//    for input edges inside the violation window. Never claimed as analog
//    voltage metastability or a real MTBF model.
bool ValidatePhase4Fault(const BoardTopology&,const Json& rule);
inline constexpr const char* CdcApproximation="functional_metastability_approx";
inline constexpr const char* UartPacketCrc8="sigflow.uart-packet-crc8.v1";
}
