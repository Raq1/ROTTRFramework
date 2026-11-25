#include "subtitle_edit.h"

#include "game_build.h"
#include "log.h"

#include <Windows.h>
#include <MinHook.h>

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace SubtitleEdit {
namespace {
// cdc::CinematicHandlerImpl::ProcessSubtitle(this, const char* record): its first bytes, up to its first call (call
// [rip + disp32]: isspace), as TR2_x64_final.exe and the retail ROTTR.exe both have them.
constexpr std::uintptr_t kRvaProcessSubtitle = 0x0016A830;
constexpr std::uint8_t kProcessSubtitleSig[] = {
    0x48, 0x89, 0x5C, 0x24, 0x20,                                // mov [rsp+20h], rbx
    0x56,                                                        // push rsi
    0x48, 0x81, 0xEC, 0x20, 0x04, 0x00, 0x00,                    // sub rsp, 420h (its 1024-byte block buffer)
    0x48, 0x8B, 0xF1,                                            // mov rsi, rcx
    0x48, 0xC7, 0x84, 0x24, 0x38, 0x04, 0x00, 0x00, 0, 0, 0, 0,  // mov qword [rsp+438h], 0 (strtok's context)
    0x0F, 0xBE, 0x0A,                                            // movsx ecx, byte [rdx]
    0x48, 0x8B, 0xDA,                                            // mov rbx, rdx
    0xFF, 0x15,                                                  // call [rip + ...]
};
// CinematicHandlerImpl +0x38: the cinematic's name (CinematicHandler::GetDebugCineName returns it): Init's, which
// MultiplexStream::ReadHeaderCallback hands it from the stream's +0x140, the name MultiplexStream::Init was given
// (strcpy): the stream's name under audio\streams\ (cinematic\<chapter>\<name>), as dtp::Multiplex m_multiplexName.
constexpr std::size_t kCineNameOffset = 0x38;

using ProcessSubtitleFn = void(__fastcall*)(void*, const char*);
ProcessSubtitleFn g_original = nullptr;

struct Choice {
    std::string text, cinematic, mod;
};
// (language, the game's line) -> the mods' lines for it, in the order the mods were read; made before the hook is
// installed, then only read.
std::map<std::pair<int, std::string>, std::vector<Choice>> g_edits;
std::set<int> g_languages;   // the languages edited
std::mutex g_logLock;
std::set<std::string> g_logged;   // (each edit logged once, when first shown)

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The handler's cinematic name (lower case), read with care (a pointer of the game's: empty when it cannot be read).
void ReadCineName(const void* handler, char* out, std::size_t size) {
    out[0] = 0;
    __try {
        const char* name = *reinterpret_cast<const char* const*>(static_cast<const std::uint8_t*>(handler) + kCineNameOffset);
        if (!name) return;
        std::size_t n = 0;
        for (; n + 1 < size && name[n]; ++n) out[n] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[n])));
        out[n] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
    }
}

// The edit for a line in the playing cutscene: the one for it (its multiplex name), else one for any cutscene.
const Choice* Pick(const std::vector<Choice>& list, const char* cine) {
    for (const Choice& c : list)
        if (!c.cinematic.empty() && c.cinematic == cine) return &c;
    for (const Choice& c : list)
        if (c.cinematic.empty()) return &c;
    return nullptr;
}

// The record with the mods' lines: read as ProcessSubtitle reads it (spaces skipped; the language number up to '\r';
// the block up to the next '\r', its lines split at '\n' as strtok splits them). false: nothing to replace.
bool Rewrite(const char* record, const char* cine, std::string& out) {
    out.clear();
    bool changed = false;
    const char* p = record;
    const char* copied = record;   // (what is not yet in out)
    while (*p && std::isspace(static_cast<unsigned char>(*p))) ++p;
    while (*p) {
        const int language = std::atoi(p);
        while (*p && *p != '\r') ++p;
        if (*p) ++p;
        // the block's lines, each looked up
        while (*p && *p != '\r') {
            const char* line = p;
            while (*p && *p != '\r' && *p != '\n') ++p;
            if (p > line && g_languages.count(language))
                if (auto it = g_edits.find({language, std::string(line, p)}); it != g_edits.end())
                    if (const Choice* c = Pick(it->second, cine)) {
                        out.append(copied, line);
                        out += c->text;
                        copied = p;
                        changed = true;
                        std::lock_guard<std::mutex> lock(g_logLock);
                        if (g_logged.size() < 2000 && g_logged.insert(c->mod + "|" + c->text).second)
                            Log::Detailf("Subtitles: %s: a subtitle line of %s worded by the mod", c->mod.c_str(),
                                         cine[0] ? cine : "a cutscene");
                    }
            if (*p == '\n') ++p;
        }
        while (*p && std::isspace(static_cast<unsigned char>(*p))) ++p;
    }
    if (!changed) return false;
    out.append(copied);
    return true;
}

void __fastcall HookProcessSubtitle(void* handler, const char* record) {
    if (!record || g_edits.empty()) return g_original(handler, record);
    char cine[260];
    ReadCineName(handler, cine, sizeof(cine));
    std::string rewritten;
    if (Rewrite(record, cine, rewritten)) return g_original(handler, rewritten.c_str());
    g_original(handler, record);
}
}  // namespace

void Add(const std::vector<Edit>& edits) {
    for (const Edit& e : edits) {
        if (e.language < 0 || e.original.empty() || e.text == e.original) continue;
        // ('\r' ends a block: the mod's text keeps to its lines)
        std::string text = e.text;
        for (char& c : text)
            if (c == '\r') c = '\n';
        g_edits[{e.language, e.original}].push_back({std::move(text), Lower(e.cinematic), e.mod});
        g_languages.insert(e.language);
    }
}

bool Install() {
    if (g_edits.empty()) return false;
    if (!GameBuild::VerifyBytes(kRvaProcessSubtitle, kProcessSubtitleSig, sizeof(kProcessSubtitleSig))) {
        Log::Write("Subtitles: the game's subtitle function is not where this framework expects it: mods' subtitles are off");
        return false;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(GameBuild::Base() + kRvaProcessSubtitle), reinterpret_cast<void*>(&HookProcessSubtitle),
                      reinterpret_cast<void**>(&g_original)) != MH_OK) {
        Log::Write("Subtitles: the subtitle hook could not be created");
        return false;
    }
    // (the Modding Suite looks for this text in the .asi to tell a framework that shows mods' subtitles)
    Log::Detailf("Subtitles: mods' cutscene subtitles are shown instead of the game's (%zu)", g_edits.size());
    return true;
}
}  // namespace SubtitleEdit
