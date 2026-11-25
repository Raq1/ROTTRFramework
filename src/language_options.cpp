// The text and voice languages switched in game (language_options.h).
#include "language_options.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "game_build.h"
#include "log.h"

namespace LanguageOptions {
namespace {
// UiMenuStateNixxesOptionsScreenAudio::OnListInitialized: ...; call [rdx+80h] (the game state); cmp eax, 2;
// lea rcx, [rbp+1D0h]; sete bl; mov [rbp+288h], bl
constexpr std::uintptr_t kRvaOnListInitialized = 0x00922050ull;
constexpr std::uint8_t kOnListInitializedSig[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
                                                  0x41, 0x56, 0x41, 0x57};
constexpr std::uintptr_t kRvaInGameCheck = 0x00922085ull;
constexpr std::uint8_t kInGameCheckSig[] = {0xFF, 0x92, 0x80, 0x00, 0x00, 0x00, 0x83, 0xF8, 0x02, 0x48, 0x8D, 0x8D,
                                            0xD0, 0x01, 0x00, 0x00, 0x0F, 0x94, 0xC3, 0x88, 0x9D, 0x88, 0x02, 0x00, 0x00};
constexpr std::uintptr_t kRvaSete = 0x00922095ull;
constexpr std::uint8_t kSete[] = {0x0F, 0x94, 0xC3};          // sete bl
constexpr std::uint8_t kMovBl1[] = {0xB3, 0x01, 0x90};        // mov bl, 1; nop (v0.9.474: was xor bl, bl)

bool g_installed = false;

bool WriteCode(std::uintptr_t rva, const std::uint8_t* bytes, std::size_t size) {
    void* at = reinterpret_cast<void*>(GameBuild::Base() + rva);
    DWORD old = 0, ignored = 0;
    if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(at, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), at, size);
    VirtualProtect(at, size, old, &ignored);
    return true;
}
}  // namespace

bool Install() {
    if (g_installed) return true;
    if (!GameBuild::IsSupportedRetail() ||
        !GameBuild::VerifyBytes(kRvaOnListInitialized, kOnListInitializedSig, sizeof(kOnListInitializedSig)) ||
        !GameBuild::VerifyBytes(kRvaInGameCheck, kInGameCheckSig, sizeof(kInGameCheckSig)) ||
        !GameBuild::VerifyBytes(kRvaSete, kSete, sizeof(kSete))) {
        Log::Write("LanguageOptions: unexpected code in the Audio options' list: the languages stay main-menu only");
        return false;
    }
    if (!WriteCode(kRvaSete, kMovBl1, sizeof(kMovBl1))) {
        Log::Write("LanguageOptions: the Audio options' code could not be written: the languages stay main-menu only");
        return false;
    }
    g_installed = true;
    Log::Write("LanguageOptions: Options > Audio's voice and text languages can be changed in game too");
    return true;
}

bool Installed() {
    return g_installed;
}
}  // namespace LanguageOptions
