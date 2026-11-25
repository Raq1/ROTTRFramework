#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstddef>

namespace GameBuild {
constexpr std::uint32_t kSupportedTimestamp = 0x632D2D99u;
constexpr std::uint32_t kSupportedImageSize = 0x037F0000u;

std::uintptr_t Base();
bool IsSupportedRetail();
bool VerifyBytes(std::uintptr_t rva, const std::uint8_t* bytes, std::size_t count);
}
