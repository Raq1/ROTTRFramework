#include "material_names.h"

#include "log.h"

// The Modding Suite's analysis, as it is (it reads Materials::Material / Materials::Set: only their fields are used).
#include "../tools/outfit_manager/material_params.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace MaterialNames {
namespace {
std::mutex g_mutex;
std::optional<Features::MaterialAnalysisInput> g_pending;
bool g_running = false;
Result g_result;
bool g_hasResult = false;

Widget WidgetOf(const MaterialParams::Param& p) {
    using MaterialParams::Kind;
    switch (p.kind) {
        case Kind::Color:
        case Kind::Tint: return p.components >= 3 ? Widget::Color : Widget::Values;
        case Kind::UvScale:
        case Kind::UvOffset: return Widget::Pair;
        case Kind::ViewDistance: return Widget::Distance;
        case Kind::HairHighlight: return p.components >= 4 ? Widget::Highlights : Widget::Values;
        default: return Widget::Values;
    }
}

Result Run(const Features::MaterialAnalysisInput& input) {
    Result result;
    result.address = input.address;
    result.detailVersion = input.detailVersion;
    result.done = true;
    // the live material as the Suite's reader would have read its resource
    Materials::Material material;
    material.id = 0;
    material.found = true;
    Materials::Set set;
    for (int p = 0; p < 10; ++p) {
        const auto& in = input.passes[p];
        auto& pass = material.passes[p];
        pass.present = in.present;
        pass.ps = in.shader;
        pass.constants = in.constants;
        for (const auto& slot : in.textures) {
            Materials::Slot s;
            s.texture = slot.texture;
            s.reg = slot.reg;
            s.kind = slot.kind;
            s.cls = slot.cls;
            pass.textures.push_back(s);
        }
    }
    for (const auto& shader : input.shaders) {
        Materials::Shader& s = set.shaders[shader.id];
        s.found = !shader.code.empty();
        s.code = shader.code;
    }
    for (const auto& name : input.textureNames) set.textureNames[name.id] = name.name;
    result.shaders = static_cast<int>(input.shaders.size());
    if (input.shaders.empty()) {
        result.problem = input.problem.empty() ? "No pixel shader of this material could be read." : input.problem;
        return result;
    }
    std::vector<MaterialParams::Param> params;
    std::map<std::uint32_t, MaterialParams::TextureRole> roles;
    try {
        params = MaterialParams::Analyze(material, set);
    } catch (...) {
        result.problem = "The shader analysis failed.";
        return result;
    }
    try {
        roles = MaterialParams::TextureRoles(material, set);
    } catch (...) {
        roles.clear();
    }
    for (int p = 0; p < 10; ++p)
        for (const auto& slot : material.passes[p].textures)
            if (auto it = roles.find(slot.texture); it != roles.end())
                result.slotRoles.push_back({p, slot.reg, it->second.role, it->second.why, it->second.fromShaders});
    for (const auto& p : params) {
        Param out;
        out.group = p.textureName.empty() ? "General" : p.textureName;
        if (p.texture && p.kind != MaterialParams::Kind::HairHighlight)
            if (auto it = roles.find(p.texture); it != roles.end() && it->second.fromShaders)
                out.group += "  (" + it->second.role + ")";
        out.label = p.label.empty() ? MaterialParams::KindName(p.kind) : p.label;
        out.help = p.help;
        out.note = p.note;
        out.slotName = p.slotName;
        out.widget = WidgetOf(p);
        out.components = std::max(1, std::min(4, p.components));
        out.plainNumbers = p.kind == MaterialParams::Kind::AlphaScale || p.kind == MaterialParams::Kind::NormalStrength;
        for (int c = 0; c < 4; ++c) {
            out.componentNames[c] = p.componentNames[c];
            for (const auto& l : p.where[c]) out.where[c].push_back({l.pass, l.index});
        }
        result.params.push_back(std::move(out));
    }
    if (result.params.empty() && !input.problem.empty()) result.problem = input.problem;
    return result;
}

void Worker() {
    for (;;) {
        Features::MaterialAnalysisInput input;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_pending) {
                g_running = false;
                return;
            }
            input = std::move(*g_pending);
            g_pending.reset();
        }
        Result result = Run(input);
        static int logged = 0;
        if (logged < 4) {
            ++logged;
            Log::Detailf("Material Editor: %u named parameter(s) from %d pixel shader(s)%s%s",
                        static_cast<unsigned>(result.params.size()), result.shaders, result.problem.empty() ? "" : ": ",
                        result.problem.c_str());
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_result = std::move(result);
        g_hasResult = true;
    }
}
}  // namespace

void Request(const Features::MaterialAnalysisInput& input) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pending = input;
    if (g_running) return;
    g_running = true;
    try {
        std::thread(Worker).detach();
    } catch (...) {
        g_running = false;
    }
}

bool Poll(Result& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_hasResult) return false;
    out = g_result;
    return true;
}

const char* PassName(int pass) { return MaterialParams::PassName(pass); }
}  // namespace MaterialNames
