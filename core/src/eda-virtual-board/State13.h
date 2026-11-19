#pragma once
#include <eda/api/logic_state.hpp>
#include <eda/api/Types.h>
#include <array>
namespace eda::virtual_board {
inline constexpr const char* State13Version="sigflow.state13.v1";
const std::array<uint8_t,1024>& State13Table();
uint8_t CombineState13(uint8_t lhs,uint8_t rhs);
Resolution ResolveState13(std::vector<Driver>);
Json ExplainState13(std::vector<Driver>);
uint32_t GetHighMask13(const std::vector<Driver>&);
uint32_t GetDrivenMask13(const std::vector<Driver>&);
}
