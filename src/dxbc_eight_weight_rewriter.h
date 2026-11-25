#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace DxbcEightWeight {
struct RewriteInfo {
    std::uint32_t lowWeightRegister = 0;
    std::uint32_t lowIndexRegister = 0;
    std::uint32_t highWeightRegister = 0;
    std::uint32_t highIndexRegister = 0;
    std::uint32_t oldTempCount = 0;
    std::uint32_t newTempCount = 0;
    std::uint32_t weightedOperationCount = 0;
};




// (v0.9.440) A DXBC container's checksum (the one at its bytes 4..19, over everything after them).
bool Checksum(const void* blob, std::size_t size, std::uint8_t out[16]);

bool Rewrite(const void* data,
             std::size_t size,
             std::vector<std::uint8_t>& output,
             RewriteInfo* info = nullptr,
             std::string* error = nullptr);
} 
