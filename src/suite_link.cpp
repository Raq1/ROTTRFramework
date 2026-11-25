#include "suite_link.h"

#include "custom_outfits.h"
#include "dtp_edit.h"
#include "features.h"
#include "log.h"
#include "nlohmann/json.hpp"

#include <Windows.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// Live link to the ROTTR Modding Suite (see suite_link.h).
//
// Transport: \\.\pipe\ROTTRModdingSuite (remote clients rejected, one Suite at a time), UTF-8 JSON, one object per
// line; a pipe thread reads and writes, the game thread (TickGameThread) runs the commands.
//
// Suite -> game:
//   {"cmd":"hello","suite":"2.0.25"}               -> {"t":"hello","protocol":1,"dtp":bool,"materials":bool,"outfit":N}
//   {"cmd":"dtp","id":N,"scope":"mod"|"game","size":S,"values":[[offset,"type","value","expected"],...]}
//       DTP values (DtpEdit types: "u32", "f32", "bf:u32:3:1"...; "expected": the value of the Suite's file there,
//       which the game must have before the first write; S: the body size of the Suite's file). "mod": the outfit is a
//       mod's, the values go to the built outfits' copies of DTP N (CustomOutfits::ResourceCopies: N itself when it
//       has none); "game": to DTP N (a game outfit as the game has it).
//       -> {"t":"dtp","id":N,"targets":[ids],"applied":a,"pending":p,"refused":r,"error":"..."}
//   {"cmd":"dtp_revert","id":N,"scope":...,"values":[[offset,"type"],...]}   the game's own values back
//   {"cmd":"material","id":M,"scope":...,"values":[[pass,stage,index,value],...]}   material constants (stage 0 pixel,
//       1 vertex; index: the float's index in the pass' MaterialBuffer) of every loaded copy of material M
//       -> {"t":"material","id":M,"materials":n,"applied":k,"error":"..."} once they are written (or could not be)
//   {"cmd":"material_revert","id":M,"scope":...}   its constants as the game loaded them
//   {"cmd":"hide","id":M,"hidden":bool}             the material not drawn (as an outfit toggle hides it)
//   {"cmd":"part_flags","model":N,"fingerprint":"hex","parts":P,"values":[[part,file,want],...]}   (2.0.26) a model's
//       part flags (cdc::PrimGroup flags, the part record's +0x1C) in every loaded copy of that model: the copies are
//       found by their layout (P parts; the fingerprint: FNV-1a 64 of the index count, part and mesh counts and each
//       part's first index and triangle count), as the game keeps no model ID with them. file: the flags of the Suite's
//       file (want == file: the game's own back). The bits the game sets itself at load (0x1000, 0x2000) stay the
//       game's; the render passes and shadow regions the game worked out from the flags (the parts' state) follow.
//       -> {"t":"part_flags","model":N,"models":k,"parts":n}
//   {"cmd":"clear"}                                 every edit of the Suite taken back
// Game -> Suite: {"t":"state","outfit":N,"dtp_edits":n,"materials":bool,"busy":n} every second.
namespace SuiteLink {
namespace {
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\ROTTRModdingSuite";
constexpr int kSuiteOwner = DtpEdit::kSuiteOwner;
constexpr std::size_t kMaxQueued = 4096;
constexpr DWORD kWriteTimeoutMs = 2000;
constexpr ULONGLONG kStateEveryMs = 1000;
constexpr ULONGLONG kMaterialWaitMs = 4000;       // a material's details (or the material list) at most this long

HANDLE g_stopEvent = nullptr, g_outEvent = nullptr;
std::mutex g_inMutex, g_outMutex;
std::deque<nlohmann::json> g_in;
std::deque<std::string> g_out;
std::atomic_bool g_connected{false};
std::atomic<int> g_clients{0};
std::atomic_bool g_wantsMaterials{false};
std::atomic<int> g_applied{0}, g_refused{0}, g_overrides{0};
std::atomic_bool g_clearRequested{false};   // (the menu's button: done on the game thread)
std::mutex g_errorMutex;
std::string g_lastError;

void SetError(const std::string& error) {
    std::lock_guard<std::mutex> lock(g_errorMutex);
    g_lastError = error;
}

void Send(const nlohmann::json& message) {
    std::string line;
    try {
        line = message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    } catch (...) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_outMutex);
        if (g_out.size() < kMaxQueued) g_out.push_back(std::move(line));
    }
    if (g_outEvent) SetEvent(g_outEvent);
}

// ---------- pipe thread ----------
bool WriteAll(HANDLE pipe, const std::string& data, HANDLE event) {
    OVERLAPPED ov{};
    ov.hEvent = event;
    ResetEvent(event);
    if (!WriteFile(pipe, data.data(), static_cast<DWORD>(data.size()), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING)
        return false;
    if (WaitForSingleObject(event, kWriteTimeoutMs) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &ov);
        return false;
    }
    DWORD done = 0;
    return GetOverlappedResult(pipe, &ov, &done, FALSE) && done == data.size();
}

void Session(HANDLE pipe) {
    HANDLE readEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE writeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OVERLAPPED rov{};
    rov.hEvent = readEvent;
    char buffer[16384];
    std::string pending;
    bool reading = false;
    for (;;) {
        if (!reading) {
            ResetEvent(readEvent);
            if (!ReadFile(pipe, buffer, sizeof(buffer), nullptr, &rov) && GetLastError() != ERROR_IO_PENDING) break;
            reading = true;
        }
        HANDLE waits[3] = {readEvent, g_outEvent, g_stopEvent};
        const DWORD w = WaitForMultipleObjects(3, waits, FALSE, 100);
        if (w == WAIT_OBJECT_0 + 2) break;
        if (w == WAIT_OBJECT_0) {
            DWORD got = 0;
            if (!GetOverlappedResult(pipe, &rov, &got, FALSE)) break;
            reading = false;
            pending.append(buffer, got);
            std::size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                nlohmann::json cmd = nlohmann::json::parse(line, nullptr, false);
                if (cmd.is_discarded() || !cmd.is_object()) {
                    Send({{"t", "error"}, {"msg", "bad json"}});
                    continue;
                }
                std::lock_guard<std::mutex> lock(g_inMutex);
                if (g_in.size() < kMaxQueued) g_in.push_back(std::move(cmd));
            }
            if (pending.size() > (1u << 22)) pending.clear();
        }
        std::deque<std::string> out;
        {
            std::lock_guard<std::mutex> lock(g_outMutex);
            out.swap(g_out);
        }
        bool failed = false;
        for (std::string& line : out) {
            line += '\n';
            if (!WriteAll(pipe, line, writeEvent)) {
                failed = true;
                break;
            }
        }
        if (failed) break;
    }
    if (reading) CancelIoEx(pipe, &rov);
    CloseHandle(readEvent);
    CloseHandle(writeEvent);
}

DWORD WINAPI PipeThread(LPVOID) {
    bool loggedFailure = false;
    for (;;) {
        if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) break;
        HANDLE pipe = CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1 << 20,
                                       1 << 20, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            if (!loggedFailure) Log::Writef("SuiteLink: CreateNamedPipe failed (%lu); retrying", GetLastError());
            loggedFailure = true;
            if (WaitForSingleObject(g_stopEvent, 2000) == WAIT_OBJECT_0) break;
            continue;
        }
        HANDLE connectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        OVERLAPPED ov{};
        ov.hEvent = connectEvent;
        bool connected = false;
        if (ConnectNamedPipe(pipe, &ov)) {
            connected = true;
        } else {
            const DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (err == ERROR_IO_PENDING) {
                HANDLE waits[2] = {connectEvent, g_stopEvent};
                if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
                    DWORD unused = 0;
                    connected = GetOverlappedResult(pipe, &ov, &unused, FALSE) != FALSE;
                } else {
                    CancelIoEx(pipe, &ov);
                }
            }
        }
        CloseHandle(connectEvent);
        if (connected) {
            {
                std::lock_guard<std::mutex> lock(g_outMutex);
                g_out.clear();
            }
            {
                std::lock_guard<std::mutex> lock(g_inMutex);
                g_in.clear();
            }
            g_connected.store(true);
            g_clients.fetch_add(1);
            Log::Write("SuiteLink: the Modding Suite connected");
            Session(pipe);
            g_connected.store(false);
            g_wantsMaterials.store(false);
            Log::Write("SuiteLink: the Modding Suite disconnected (its edits stay until it takes them back or the game restarts)");
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
    }
    return 0;
}

// ---------- game thread ----------
// The IDs the game has a resource under: in a mod's outfit, the built outfits' copies (N itself when none); else N.
std::vector<std::uint32_t> Targets(std::uint8_t type, std::uint32_t id, bool mod) {
    std::vector<std::uint32_t> ids;
    if (mod) {
        std::vector<std::uint32_t> copies(2048);   // (one per outfit at most; 32 before framework 0.9.404)
        const int n = CustomOutfits::ResourceCopies(type, id, copies.data(), static_cast<int>(copies.size()));
        ids.assign(copies.begin(), copies.begin() + n);
    }
    if (ids.empty()) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

bool ModScope(const nlohmann::json& cmd) { return cmd.value("scope", std::string("game")) == "mod"; }

void RunDtp(const nlohmann::json& cmd, bool revert) {
    const std::uint32_t id = cmd.value("id", 0u);
    const std::uint32_t size = cmd.value("size", 0u);
    const auto targets = Targets(7, id, ModScope(cmd));
    int applied = 0, pending = 0, refused = 0;
    std::string error;
    const auto& values = cmd.contains("values") && cmd["values"].is_array() ? cmd["values"] : nlohmann::json::array();
    for (const std::uint32_t target : targets)
        for (const auto& v : values) {
            if (!v.is_array() || v.size() < 2 || !v[0].is_number_unsigned() || !v[1].is_string()) continue;
            DtpEdit::Field field;
            std::string why;
            if (!DtpEdit::ParseField(target, v[0].get<std::uint32_t>(), v[1].get<std::string>(), field, why)) {
                ++refused;
                if (error.empty()) error = why;
                continue;
            }
            if (revert) {
                DtpEdit::ClearOverride(kSuiteOwner, field);
                ++applied;
                continue;
            }
            if (v.size() < 3 || !v[2].is_string()) continue;
            std::vector<std::string> expect;
            if (v.size() >= 4 && v[3].is_string()) expect.push_back(v[3].get<std::string>());
            switch (DtpEdit::SetOverride(kSuiteOwner, field, v[2].get<std::string>(), expect, size, why)) {
                case DtpEdit::SetResult::Applied: ++applied; break;
                case DtpEdit::SetResult::Pending: ++pending; break;
                case DtpEdit::SetResult::Refused:
                    ++refused;
                    if (error.empty()) error = why;
                    break;
            }
        }
    g_applied.fetch_add(applied);
    g_refused.fetch_add(refused);
    if (!error.empty()) SetError(error);
    Send({{"t", revert ? "dtp_revert" : "dtp"}, {"id", id}, {"targets", targets}, {"applied", applied}, {"pending", pending},
          {"refused", refused}, {"error", error}});
}

// Material edits wait for their material's details (the Material Editor uploads the constants of the material whose
// details are published), one material at a time.
struct MaterialEdit {
    int pass = 0, stage = 0;
    std::uint32_t index = 0;
    float value = 0;
};
struct MaterialJob {
    std::uint32_t id = 0;            // the Suite's material ID
    bool mod = false;
    bool restore = false;
    std::vector<MaterialEdit> edits;
    std::vector<std::uintptr_t> addresses;   // its loaded copies (found when the job starts)
    std::size_t next = 0;            // the copy being written
    bool requested = false;
    ULONGLONG since = 0;
    int applied = 0;
};
std::deque<MaterialJob> g_materialJobs;
std::set<std::pair<std::uint32_t, bool>> g_editedMaterials;   // (id, mod) the Suite changed: taken back by "clear"

void QueueMaterial(const nlohmann::json& cmd, bool restore) {
    MaterialJob job;
    job.id = cmd.value("id", 0u);
    job.mod = ModScope(cmd);
    job.restore = restore;
    if (!restore && cmd.contains("values") && cmd["values"].is_array())
        for (const auto& v : cmd["values"]) {
            if (!v.is_array() || v.size() < 4 || !v[3].is_number()) continue;
            job.edits.push_back({v[0].get<int>(), v[1].get<int>(), v[2].get<std::uint32_t>(), v[3].get<float>()});
        }
    if (!job.id || (!restore && job.edits.empty())) return;
    if (restore) g_editedMaterials.erase({job.id, job.mod});
    else g_editedMaterials.insert({job.id, job.mod});
    // a newer edit of the same material replaces a waiting one's values (sliders send many)
    for (auto& waiting : g_materialJobs)
        if (&waiting != &g_materialJobs.front() && waiting.id == job.id && waiting.mod == job.mod && !waiting.restore && !restore) {
            for (const auto& e : job.edits) {
                auto it = std::find_if(waiting.edits.begin(), waiting.edits.end(), [&](const MaterialEdit& w) {
                    return w.pass == e.pass && w.stage == e.stage && w.index == e.index;
                });
                if (it != waiting.edits.end()) *it = e;
                else waiting.edits.push_back(e);
            }
            return;
        }
    g_materialJobs.push_back(std::move(job));
    g_wantsMaterials.store(true);
}

void TickMaterials() {
    if (g_materialJobs.empty()) return;
    MaterialJob& job = g_materialJobs.front();
    const ULONGLONG now = GetTickCount64();
    auto finish = [&](const std::string& error) {
        if (!error.empty()) SetError(error);
        Send({{"t", job.restore ? "material_revert" : "material"}, {"id", job.id}, {"materials", job.addresses.size()},
              {"applied", job.applied}, {"error", error}});
        g_materialJobs.pop_front();
    };
    if (!Features::MaterialEditorAvailable()) {
        finish("the Material Editor is not available in this game build");
        return;
    }
    if (!job.since) job.since = now;
    if (job.addresses.empty()) {
        // its loaded copies, from the Material Editor's list (built while it is active)
        const auto ids = Targets(10, job.id, job.mod);
        static std::vector<Features::MaterialInfo> list;
        list.resize(std::size_t(std::max(1, Features::MaterialCount())) + 64);
        const int count = Features::CopyMaterials(list.data(), int(list.size()));
        for (int i = 0; i < count; ++i)
            if (std::find(ids.begin(), ids.end(), std::uint32_t(list[std::size_t(i)].id)) != ids.end())
                job.addresses.push_back(list[std::size_t(i)].address);
        if (job.addresses.empty()) {
            if (now - job.since > kMaterialWaitMs) finish("material " + std::to_string(job.id) + " is not loaded in the game now");
            else if (!count) Features::RequestMaterialRefresh();
            return;
        }
    }
    if (job.next >= job.addresses.size()) {
        finish("");
        return;
    }
    const std::uintptr_t address = job.addresses[job.next];
    if (job.restore) {
        if (Features::RequestMaterialRestore(address)) ++job.applied;
        ++job.next;
        return;
    }
    Features::MaterialDetails details;
    if (Features::GetMaterialDetails(details) && details.address == address) {
        for (std::size_t i = 0; i < job.edits.size(); ++i) {
            const MaterialEdit& e = job.edits[i];
            if (Features::QueueMaterialConstantUpdate(address, e.pass, e.stage, e.index, e.value, i + 1 == job.edits.size())) ++job.applied;
        }
        ++job.next;
        job.requested = false;
        job.since = now;
        return;
    }
    if (!job.requested) {
        job.requested = Features::RequestMaterialDetails(address);
        job.since = now;
        if (!job.requested) ++job.next;   // (not in the list any more)
    } else if (now - job.since > kMaterialWaitMs) {
        ++job.next;   // (its details did not come: the next copy)
        job.requested = false;
    }
}

void Hide(const nlohmann::json& cmd) {
    const std::uint32_t id = cmd.value("id", 0u);
    const bool hidden = cmd.value("hidden", false);
    bool ok = Features::MaterialResourceVisibilityAvailable();
    if (ok) {
        Features::SetMaterialResourceHidden(id, hidden);   // (its renamed copies too)
        std::vector<std::uint32_t> copies(2048);   // (one per outfit at most)
        const int n = CustomOutfits::ResourceCopies(10, id, copies.data(), static_cast<int>(copies.size()));
        for (int i = 0; i < n; ++i) Features::SetMaterialResourceHiddenOne(copies[static_cast<std::size_t>(i)], hidden);
    }
    Send({{"t", "hide"}, {"id", id}, {"hidden", hidden}, {"ok", ok}});
}

void SendState() {
    Send({{"t", "state"},
          {"outfit", Features::CurrentOutfitObjectId()},
          {"dtp_edits", DtpEdit::OverrideCount(kSuiteOwner)},
          {"materials", Features::MaterialEditorAvailable()},
          {"busy", g_materialJobs.size()}});
}

// ---- part flags (2.0.26) ----
// The model data a CommonRenderModelInstance draws (its mesh's vtable +0x70, as CommonRenderModelInstance::
// GetPrimGroupRenderPassesMask asks it): the model resource's header in memory ("Mesh"), +0x60 -1 while not ready,
// +0x88 the part records (0x60 each), +0xB0 their count, +0x0C the index count, +0xB2 the mesh count. The instance's
// prim group state (+0x18, 0x10 a part: +8 the render passes, +0xC the shadow regions; the mesh's shared state, or the
// instance's own copy) was worked out from the flags by CommonRenderModel::FinalizeInitialization.
constexpr std::uint32_t kEngineFlags = 0x1000 | 0x2000;   // kTessellationUpgrade / kTessellationHide: the game's
constexpr std::size_t kPartRecord = 0x60;
constexpr std::size_t kPartState = 0x10;

struct PartFlagEdit {
    std::uint32_t file = 0, want = 0;
};
struct ModelFlagEdit {
    std::uint32_t model = 0;   // the Suite's model ID (for the log)
    std::uint32_t parts = 0;
    std::map<std::uint32_t, PartFlagEdit> edits;   // part -> flags
};
std::map<std::uint64_t, ModelFlagEdit> g_partFlags;   // layout fingerprint ->
ULONGLONG g_lastPartFlagsMs = 0;

bool ReadU32(const std::uint8_t* p, std::uint32_t& out) {
    __try {
        out = *reinterpret_cast<const std::uint32_t*>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteU32(std::uint8_t* p, std::uint32_t value) {
    __try {
        *reinterpret_cast<std::uint32_t*>(p) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The instance's model data and prim group state (null: not ready / not readable).
bool InstanceModel(void* rmi, std::uint8_t*& model, std::uint8_t*& state) {
    __try {
        auto* bytes = static_cast<std::uint8_t*>(rmi);
        void* mesh = *reinterpret_cast<void**>(bytes + 0x48);
        if (!mesh) return false;
        using GetModelFn = std::uint8_t*(__fastcall*)(void*);
        auto** vtable = *reinterpret_cast<void***>(mesh);
        model = reinterpret_cast<GetModelFn>(vtable[14])(mesh);
        if (!model || *reinterpret_cast<const std::int32_t*>(model + 0x60) == -1) return false;
        state = *reinterpret_cast<std::uint8_t**>(bytes + 0x18);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// FNV-1a 64, a u32's bytes (the Suite's LiveLink computes the same from the model file)
void Mix(std::uint64_t& h, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) {
        h ^= (v >> (8 * k)) & 0xFF;
        h *= 0x100000001B3ull;
    }
}

bool ModelLayout(const std::uint8_t* model, std::uint32_t& parts, std::uint8_t*& records, std::uint64_t& fingerprint) {
    __try {
        if (*reinterpret_cast<const std::uint32_t*>(model) != 0x6873654Du) return false;   // "Mesh"
        parts = *reinterpret_cast<const std::uint16_t*>(model + 0xB0);
        records = *reinterpret_cast<std::uint8_t* const*>(model + 0x88);
        if (!records || parts == 0 || parts > 4096) return false;
        std::uint64_t h = 0xCBF29CE484222325ull;
        Mix(h, *reinterpret_cast<const std::uint32_t*>(model + 0x0C));
        Mix(h, parts);
        Mix(h, *reinterpret_cast<const std::uint16_t*>(model + 0xB2));
        for (std::uint32_t p = 0; p < parts; ++p) {
            Mix(h, *reinterpret_cast<const std::uint32_t*>(records + p * kPartRecord + 0x10));
            Mix(h, *reinterpret_cast<const std::uint32_t*>(records + p * kPartRecord + 0x14));
        }
        fingerprint = h;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// As CommonRenderModel::FinalizeInitialization: the render passes (bit 0 no normal draw, bit 1 no shadow, the model's
// own no-shadow bit +4 & 2) and the shadow regions (bits 0x30: that many + 1, from the last region).
std::uint32_t PassesOf(std::uint32_t flags, std::uint32_t modelFlags) {
    const std::uint32_t both = (modelFlags & 2) | flags;
    std::uint32_t off = (flags & 1) ? 0xFFFFFF9Fu : 0u;
    off |= (both & 2) << 5;
    return ~off;
}

std::uint32_t RegionsOf(std::uint32_t flags) {
    const int n = int((flags >> 4) & 3) + 1;
    std::uint32_t mask = 0;
    for (int k = 0; k < n; ++k) mask |= 1u << (3 - k);
    return mask;
}

struct FlagChange {
    std::uint32_t part, before, after;
};
struct FlagPass {
    std::map<const std::uint8_t*, std::vector<FlagChange>> models;   // model data -> what changed in it this pass
    std::set<std::uint8_t*> states;
    int copies = 0, parts = 0;
};

void FlagInstance(void* rmi, void* context) {
    auto& pass = *static_cast<FlagPass*>(context);
    std::uint8_t* model = nullptr;
    std::uint8_t* state = nullptr;
    if (!InstanceModel(rmi, model, state)) return;
    auto found = pass.models.find(model);
    if (found == pass.models.end()) {   // the first instance of this model data: its records
        std::vector<FlagChange> changes;
        std::uint32_t parts = 0;
        std::uint8_t* records = nullptr;
        std::uint64_t fingerprint = 0;
        if (ModelLayout(model, parts, records, fingerprint)) {
            const auto edit = g_partFlags.find(fingerprint);
            if (edit != g_partFlags.end() && edit->second.parts == parts)
                for (const auto& [part, e] : edit->second.edits) {
                    if (part >= parts) continue;
                    std::uint32_t now = 0;
                    if (!ReadU32(records + part * kPartRecord + 0x1C, now)) continue;
                    const std::uint32_t want = (now & kEngineFlags) | (e.want & ~kEngineFlags);
                    if (want == now || !WriteU32(records + part * kPartRecord + 0x1C, want)) continue;
                    changes.push_back({part, now, want});
                }
            if (!changes.empty()) {
                ++pass.copies;
                pass.parts += int(changes.size());
            }
        }
        found = pass.models.emplace(model, std::move(changes)).first;
    }
    if (found->second.empty() || !state || !pass.states.insert(state).second) return;
    std::uint32_t modelFlags = 0;
    ReadU32(model + 4, modelFlags);
    for (const FlagChange& c : found->second) {   // the passes and regions the game worked out, unless changed since
        std::uint8_t* s = state + c.part * kPartState;
        std::uint32_t passes = 0, regions = 0;
        if (ReadU32(s + 8, passes) && passes == PassesOf(c.before, modelFlags)) WriteU32(s + 8, PassesOf(c.after, modelFlags));
        if (ReadU32(s + 0xC, regions) && regions == RegionsOf(c.before)) WriteU32(s + 0xC, RegionsOf(c.after));
    }
}

// Every loaded copy of the edited models brought in line (on a command, and every 500 ms for copies loaded since).
FlagPass ApplyPartFlags() {
    FlagPass pass;
    if (!g_partFlags.empty()) Features::ForEachLiveModelInstance(&FlagInstance, &pass);
    // edits back at the file's flags are done once applied (the game loads the file's flags anyway)
    for (auto it = g_partFlags.begin(); it != g_partFlags.end();) {
        auto& edits = it->second.edits;
        for (auto e = edits.begin(); e != edits.end();) e = e->second.want == e->second.file ? edits.erase(e) : std::next(e);
        it = edits.empty() ? g_partFlags.erase(it) : std::next(it);
    }
    return pass;
}

void PartFlags(const nlohmann::json& cmd) {
    const std::uint32_t model = cmd.value("model", 0u);
    const std::uint64_t fingerprint = std::strtoull(cmd.value("fingerprint", std::string("0")).c_str(), nullptr, 16);
    ModelFlagEdit& edit = g_partFlags[fingerprint];
    edit.model = model;
    edit.parts = cmd.value("parts", 0u);
    if (cmd.contains("values") && cmd["values"].is_array())
        for (const auto& v : cmd["values"]) {
            if (!v.is_array() || v.size() < 3 || !v[0].is_number_unsigned() || !v[1].is_number_unsigned() || !v[2].is_number_unsigned())
                continue;
            edit.edits[v[0].get<std::uint32_t>()] = PartFlagEdit{v[1].get<std::uint32_t>(), v[2].get<std::uint32_t>()};
        }
    const FlagPass pass = ApplyPartFlags();
    Log::Detailf("SuiteLink: model %u part flags: %d part%s changed in %d loaded cop%s", model, pass.parts, pass.parts == 1 ? "" : "s",
                pass.copies, pass.copies == 1 ? "y" : "ies");
    Send({{"t", "part_flags"}, {"model", model}, {"models", pass.copies}, {"parts", pass.parts}});
}

void ClearPartFlags() {
    for (auto& [fingerprint, edit] : g_partFlags)
        for (auto& [part, e] : edit.edits) e.want = e.file;
    ApplyPartFlags();
    g_partFlags.clear();
}

ULONGLONG g_lastStateMs = 0;
bool g_wasConnected = false;
}  // namespace

bool Initialize() {
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_outEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE thread = CreateThread(nullptr, 0, PipeThread, nullptr, 0, nullptr);
    if (!thread) {
        Log::Write("SuiteLink: the pipe thread could not start");
        return false;
    }
    CloseHandle(thread);
    Log::Detail("SuiteLink: waiting for the Modding Suite on \\\\.\\pipe\\ROTTRModdingSuite");
    return true;
}

bool WantsMaterials() { return g_connected.load() && g_wantsMaterials.load(); }

void TickGameThread() {
    const bool connected = g_connected.load();
    if (connected != g_wasConnected) {
        g_wasConnected = connected;
        if (!connected) g_materialJobs.clear();
    }
    // the Material Editor's capture while the Suite edits materials (the overlay sets it again while its own is open)
    static bool materialsOn = false;
    const bool wants = WantsMaterials();
    if (wants) Features::SetMaterialEditorActive(true);
    else if (materialsOn) Features::SetMaterialEditorActive(false);
    materialsOn = wants;
    std::deque<nlohmann::json> commands;
    {
        std::lock_guard<std::mutex> lock(g_inMutex);
        commands.swap(g_in);
    }
    for (const auto& cmd : commands) {
        const std::string name = cmd.value("cmd", std::string());
        try {
            if (name == "hello") {
                Send({{"t", "hello"},
                      {"protocol", 1},
                      {"dtp", DtpEdit::Available()},
                      {"materials", Features::MaterialEditorAvailable()},
                      {"hide", Features::MaterialResourceVisibilityAvailable()},
                      {"outfit", Features::CurrentOutfitObjectId()}});
                Log::Detailf("SuiteLink: hello from the Modding Suite %s", cmd.value("suite", std::string("?")).c_str());
            } else if (name == "dtp") {
                RunDtp(cmd, false);
            } else if (name == "dtp_revert") {
                RunDtp(cmd, true);
            } else if (name == "material") {
                QueueMaterial(cmd, false);
            } else if (name == "material_revert") {
                QueueMaterial(cmd, true);
            } else if (name == "hide") {
                Hide(cmd);
            } else if (name == "part_flags") {
                PartFlags(cmd);
            } else if (name == "clear") {
                g_clearRequested.store(true);
                Send({{"t", "cleared"}});
            } else {
                Send({{"t", "error"}, {"msg", "unknown command: " + name}});
            }
        } catch (const std::exception& e) {
            Send({{"t", "error"}, {"msg", std::string(name) + ": " + e.what()}});
        }
    }
    if (g_clearRequested.exchange(false)) {
        DtpEdit::ClearOwner(kSuiteOwner);
        ClearPartFlags();
        const auto edited = g_editedMaterials;
        for (const auto& [id, mod] : edited) QueueMaterial({{"id", id}, {"scope", mod ? "mod" : "game"}}, true);
    }
    TickMaterials();
    if (!g_partFlags.empty() && GetTickCount64() - g_lastPartFlagsMs >= 500) {   // copies loaded since
        g_lastPartFlagsMs = GetTickCount64();
        ApplyPartFlags();
    }
    if (g_materialJobs.empty() && g_editedMaterials.empty()) g_wantsMaterials.store(false);
    g_overrides.store(DtpEdit::OverrideCount(kSuiteOwner));
    const ULONGLONG now = GetTickCount64();
    if (connected && now - g_lastStateMs >= kStateEveryMs) {
        g_lastStateMs = now;
        SendState();
    }
}

}  // namespace SuiteLink
