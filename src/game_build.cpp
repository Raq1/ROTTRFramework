#include "game_build.h"
#include <cstring>

namespace GameBuild {
std::uintptr_t Base() {
    return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
}

bool IsSupportedRetail() {
    const auto base = Base();
    if (!base) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    return nt->FileHeader.TimeDateStamp == kSupportedTimestamp &&
           nt->OptionalHeader.SizeOfImage == kSupportedImageSize;
}

bool VerifyBytes(std::uintptr_t rva, const std::uint8_t* bytes, std::size_t count) {
    if (!bytes || count == 0) return false;
    const void* at = reinterpret_cast<const void*>(Base() + rva);
    return std::memcmp(at, bytes, count) == 0;
}
}
