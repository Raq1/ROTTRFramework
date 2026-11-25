// (v0.9.542) The crash in DialogueManager::DialoguePassedAllStartingRequirements after an outfit switch (retail RVAs of
// the Steam ROTTR.exe, from TR2_x64_final's functions of the same names; each address byte-checked).
//
// The user's crash (0.9.541): an access violation at ROTTR.exe+0x5555A8 (read 0xD9), from a level script's dialogue
// node (AGDialogueDialogueNode::ProcessMsg -> DialogueManager::TryToAddActionGraphDialogueToPipeline ->
// IsPossibleToPlaySequenceSoon -> DialoguePassedAllStartingRequirements, TR2 0x140588540). For each required actor it
// finds the actor's ActorData in the manager's per-speaker lists (DialogueManager +0x370 + id * 8, 512 of them, retail;
// TR2 +0x380: a sentinel node, nodes linked by +0, their ActorData at +0x10) by the instance's placement hash
// (Instance +0x60 = ActorData +0x20) and reads the dialogue the actor is in: ActorData +0x10 -> its sequence (+0) ->
// +0xD4. That ActorData was Lara's (hash 0x2C3D23EC7F55792A, its handle 0xFAF1FDF500000002: the instance the outfit
// switch had just made), its dialogue pointer at freed memory (holding 5).
// - DialogueManager::ClearDialogueReferencesForActors (TR2 0x140587400), when a dialogue ends, clears each actor's
//   ActorData +0x10 only for actors whose instance it can still get (Dialogue::GetDialogueActorInstance); an actor
//   destroyed while in a dialogue keeps it, and the dialogue is freed.
// - DialogueManager::RegisterActor(instance, id) (TR2 0x14058ED80, retail 0x55BA20), for an ActorData of the same
//   placement hash and id, only writes the instance's handle (Instance +0x68) into ActorData +0 and keeps the rest.
// The game makes Lara's instance again only with a reload (which drops all actor data, DeleteAllActorData); the
// framework's outfit switch destroys her mid-level (here during her death, while a dialogue had her) and makes her again
// with the same placement hash, so her new instance inherited the freed dialogue.
// Now RegisterActor is hooked: before the game's, the ActorData it will take over (same hash, same id) whose handle is
// another instance's, and that instance is gone (its pool slot, retail 0x24EEAD0 + slot * 0x120, handle +0x118, no
// longer holds that handle), has its dialogue reference cleared (logged). A live other instance's is left alone.
#include "dialogue_guard.h"

#include "game_build.h"
#include "log.h"

#include <MinHook.h>
#include <Windows.h>

#include <atomic>
#include <cstdint>

namespace DialogueGuard {
namespace {
constexpr std::uintptr_t kRvaRegisterActor = 0x0055BA20ull, kRvaInstancePool = 0x024EEAD0ull;
// push rbp; push rdi; push r14; sub rsp, 30h; mov r14d, r8d; mov rdi, rdx; mov rbp, rcx; test rdx, rdx; je ...;
// cmp qword [rcx+328h], 0
constexpr std::uint8_t kRegisterActorSig[] = {0x40, 0x55, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x45, 0x8B,
                                              0xF0, 0x48, 0x8B, 0xFA, 0x48, 0x8B, 0xE9, 0x48, 0x85, 0xD2, 0x0F,
                                              0x84, 0x7B, 0x04, 0x00, 0x00, 0x48, 0x83, 0xB9, 0x28, 0x03};
constexpr std::size_t kMgrActorLists = 0x370, kInstanceHash = 0x60, kInstanceHandle = 0x68;
constexpr std::size_t kActorHandle = 0x00, kActorDialogue = 0x10, kActorId = 0x18, kActorHash = 0x20;
constexpr std::uint32_t kActorLists = 0x200, kPoolSlots = 0x7530;
constexpr std::size_t kPoolStride = 0x120, kPoolHandle = 0x118;

using RegisterActorFn = bool(__fastcall*)(void* manager, void* instance, std::uint32_t id);
RegisterActorFn g_originalRegisterActor = nullptr;
std::atomic<int> g_logs{0};

bool HandleLive(std::uint64_t handle) {
    const std::uint32_t slot = static_cast<std::uint32_t>(handle);
    if (!handle || !slot || slot >= kPoolSlots) return false;
    const auto* entry = reinterpret_cast<const std::uint8_t*>(GameBuild::Base() + kRvaInstancePool) + slot * kPoolStride;
    return *reinterpret_cast<const std::uint64_t*>(entry + kPoolHandle) == handle;
}

// The ActorData RegisterActor will take over for this instance and id: its dialogue reference cleared when it was
// another instance's and that one is gone.
void DropStaleDialogue(void* manager, void* instance, std::uint32_t id) noexcept {
    __try {
        auto* m = static_cast<std::uint8_t*>(manager);
        auto* inst = static_cast<std::uint8_t*>(instance);
        const std::uint64_t hash = *reinterpret_cast<std::uint64_t*>(inst + kInstanceHash);
        const std::uint64_t handle = *reinterpret_cast<std::uint64_t*>(inst + kInstanceHandle);
        void* head = *reinterpret_cast<void**>(m + kMgrActorLists + static_cast<std::size_t>(id) * 8);
        if (!head) return;
        int walked = 0;
        for (void* node = *static_cast<void**>(head); node && node != head && walked < 4096;
             node = *static_cast<void**>(node), ++walked) {
            auto* data = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(node) + 0x10);
            if (!data || *reinterpret_cast<std::uint64_t*>(data + kActorHash) != hash ||
                *reinterpret_cast<std::uint32_t*>(data + kActorId) != id)
                continue;
            const std::uint64_t old = *reinterpret_cast<std::uint64_t*>(data + kActorHandle);
            void*& dialogue = *reinterpret_cast<void**>(data + kActorDialogue);
            if (old == handle || !dialogue || HandleLive(old)) return;
            if (g_logs.fetch_add(1) < 16)
                Log::Writef("DialogueGuard: speaker %u (placement hash 0x%016llX) made again as instance handle 0x%016llX; "
                            "its previous instance's (0x%016llX, gone) dialogue reference %p dropped (that dialogue may be "
                            "freed: DialoguePassedAllStartingRequirements would read it)", id,
                            static_cast<unsigned long long>(hash), static_cast<unsigned long long>(handle),
                            static_cast<unsigned long long>(old), dialogue);
            dialogue = nullptr;
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool __fastcall HookRegisterActor(void* manager, void* instance, std::uint32_t id) {
    if (manager && instance && id < kActorLists) DropStaleDialogue(manager, instance, id);
    return g_originalRegisterActor(manager, instance, id);
}
}  // namespace

bool Install() {
    if (!GameBuild::IsSupportedRetail()) return false;
    if (!GameBuild::VerifyBytes(kRvaRegisterActor, kRegisterActorSig, sizeof(kRegisterActorSig))) {
        Log::Write("DialogueGuard: DialogueManager::RegisterActor does not match this build; no guard");
        return false;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(GameBuild::Base() + kRvaRegisterActor), reinterpret_cast<void*>(&HookRegisterActor),
                      reinterpret_cast<void**>(&g_originalRegisterActor)) != MH_OK) {
        Log::Write("DialogueGuard: the RegisterActor hook could not be created; no guard");
        return false;
    }
    Log::Write("DialogueGuard: ready (a speaker made again drops its previous instance's dialogue reference)");
    return true;
}
}  // namespace DialogueGuard
