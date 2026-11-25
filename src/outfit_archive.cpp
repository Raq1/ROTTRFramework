#include "outfit_archive.h"

#include "nlohmann/json.hpp"
#include "game_archive.h"
#include "image_load.h"


#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>
#include <tuple>
#include <unordered_map>

namespace fs = std::filesystem;

namespace OutfitArchive {
namespace {
using namespace GameArchive;
constexpr std::uint64_t kBuilderVersion = 23;   // bump when the output layout changes (forces a rebuild); 5: ZoneData;
                                                // 15: the models of the object data's meshrefs carried; 16: the
                                                // copied resources listed in the .nfo (the Suite's live edits);
                                                // 17: pictures of other sizes keep the template's texture header;
                                                // 18: and its mip level count (0); 20: and its asset path field;
                                                // 21: the pictures' texture IDs and sizes in the .nfo; 22: past 64
                                                // pictures, the ones wider than 256 halved down to it; 23: a game
                                                // .drm that is no object (font libraries) rebuilt in place (v0.9.602)


// Types whose data can refer to other resources (scanned for references to replaced resources).
bool CanReference(std::uint8_t type) {
    switch (type) {
        case kTypeBlendShapeDriver: case kTypeDtp: case kTypeMaterial: case kTypeContentRef: case kTypeModel:
        case kTypeCollisionModel: case kTypeObjectRef: case kTypeAnimationLib: case 16:
            return true;
        default:
            return false;
    }
}

// The models (and collision models) the object data's meshref scene items name by plain ID (dtp::MeshRef
// m_meshRenderID +0xC, m_meshCollisionID +0x10: not references, so nothing else brings them into the .drm; the game
// finds them by ID among the loaded resources). dtp::ZoneData: m_numItemLists +0x38, m_itemLists +0x40
// (dtp::ZoneItemDataList, 16: m_listID, m_numItems, m_items); dtp::ZoneItemData (0x70): m_properties +0x60.
std::vector<Key> MeshRefResources(const std::vector<std::uint8_t>& data) {
    std::vector<Key> keys;
    const std::uint64_t refSize = RefDefinitionsSize(data);
    if (refSize > data.size()) return keys;
    const std::size_t body = static_cast<std::size_t>(refSize);
    const std::uint32_t internals = Get<std::uint32_t>(data, 0);
    std::unordered_map<std::uint32_t, std::uint32_t> pointers;
    for (std::uint32_t i = 0; i < internals && 28 + 8 * std::size_t(i) <= data.size(); ++i)
        pointers[Get<std::uint32_t>(data, 20 + 8 * std::size_t(i))] = Get<std::uint32_t>(data, 24 + 8 * std::size_t(i));
    auto u32 = [&](std::size_t at) -> std::uint32_t { return body + at + 4 <= data.size() ? Get<std::uint32_t>(data, body + at) : 0; };
    auto target = [&](std::uint32_t slot, std::uint32_t& out) {
        auto it = pointers.find(slot);
        if (it == pointers.end()) return false;
        out = it->second;
        return true;
    };
    constexpr std::uint32_t kMeshRefList = 0x5F8E3154u;   // simple hash of "meshref"
    std::uint32_t lists = 0;
    if (!target(0x40, lists)) return keys;
    for (std::uint32_t i = 0, n = std::min<std::uint32_t>(u32(0x38), 4096); i < n; ++i) {
        const std::uint32_t list = lists + i * 16;
        std::uint32_t items = 0;
        if (u32(list) != kMeshRefList || !target(list + 8, items)) continue;
        for (std::uint32_t k = 0, count = std::min<std::uint32_t>(u32(list + 4), 4096); k < count; ++k) {
            std::uint32_t properties = 0;
            if (!target(items + k * 0x70 + 0x60, properties)) continue;
            if (const std::uint32_t model = u32(properties + 0xC)) keys.push_back(Key{kTypeModel, model});
            if (const std::uint32_t collision = u32(properties + 0x10)) keys.push_back(Key{kTypeCollisionModel, collision});
        }
    }
    return keys;
}

// ---- user files -------------------------------------------------------------------------------------------------------

struct ExtensionType {
    const char* extension;
    std::uint8_t type;
};
constexpr ExtensionType kExtensions[] = {
    {".tr10anim", kTypeAnimation},       {".tr10animlib", kTypeAnimationLib}, {".tr10cmodel", kTypeCollisionModel},
    {".tr10dtp", kTypeDtp},              {".tr10layer", kTypeDtp},            {".tr10contentref", kTypeContentRef},
    {".tr10material", kTypeMaterial},    {".tr10modeldata", kTypeModel},      {".tr10model", kTypeModel},
    {".tr10cubelut", kTypeModel},        {".tr10shresource", kTypeModel},     {".tr10objectref", kTypeObjectRef},
    {".tr10drivers", kTypeBlendShapeDriver}, {".tr10script", kTypeScript},    {".tr10shaderlib", kTypeShaderLib},
    {".tr10sound", kTypeSoundBank},      {".dds", kTypeTexture},
};

// "267210.tr10modeldata", "lara_boots_metal.178450.tr10material", "1485402.dds" -> type and ID.
bool ParseResourceFileName(const fs::path& path, Key& key) {
    const std::string extension = Lower(path.extension().string());
    std::uint8_t type = 0;
    for (const auto& e : kExtensions)
        if (extension == e.extension) type = e.type;
    if (!type) return false;
    const std::string stem = path.stem().string();
    const std::size_t dot = stem.find_last_of('.');
    const std::string number = dot == std::string::npos ? stem : stem.substr(dot + 1);
    if (number.empty() || number.size() > 9 || !std::all_of(number.begin(), number.end(), ::isdigit)) return false;
    key = {type, static_cast<std::uint32_t>(std::stoul(number))};
    return true;
}

bool ReadDisk(const fs::path& path, std::vector<std::uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    out.resize(static_cast<std::size_t>(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(in);
}


// ---- archive writer ---------------------------------------------------------------------------------------------------

class ArchiveWriter {
public:
    explicit ArchiveWriter(int id) : id_(id) {}
    ~ArchiveWriter() {
        if (file_.is_open()) {
            file_.close();
            std::error_code ec;
            fs::remove(path_, ec);   // (not saved: the partial file goes)
        }
    }

    // The archive is written while it is built: every resource straight to the file (framework 0.9.403; before, the
    // whole archive was kept in memory and then copied once more to be written: 1.1 GB twice for 107 outfits, and
    // every copy of an outfit adds its own object data, 2.5 MB for laracroft). files: how many files (.drm) it will
    // have at most; the file table before the resources has room for them (the same layout when all are built).
    bool Open(const fs::path& path, std::size_t files, std::string& error) {
        path_ = path;
        start_ = (0x14 + 0x20 + files * sizeof(FileEntry) + 0xF) & ~std::size_t(0xF);
        file_.open(path, std::ios::binary | std::ios::trunc);
        if (!file_) {
            error = "cannot write " + path.string();
            return false;
        }
        const std::vector<char> zeros(start_, 0);   // (the header and table, written at the end)
        file_.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
        return static_cast<bool>(file_);
    }
    // Where the resources start: their locations are relative to it until the caller adds it.
    std::size_t Start() const { return start_; }

    // Stores one resource (uncompressed CDRM chunks, like the mod manager's fallback). Returns its location.
    DrmLocation AddResource(const std::vector<std::uint8_t>& data, Key key) {
        // the previous resource's NEXT block says that another follows (0x10: the distance to it)
        if (hasResources_) WriteNext(0x10);
        const std::uint64_t resourceAt = contentSize_;
        const std::uint32_t chunks = static_cast<std::uint32_t>((data.size() + kMaxChunk - 1) / kMaxChunk);
        std::vector<std::uint8_t> head;
        Append<std::uint32_t>(head, kCdrmMagic);
        Append<std::uint32_t>(head, 0);
        Append<std::uint32_t>(head, chunks);
        Append<std::uint32_t>(head, 0);
        for (std::uint32_t c = 0; c < chunks; ++c) {
            const std::uint32_t size = static_cast<std::uint32_t>(std::min<std::size_t>(kMaxChunk, data.size() - std::size_t(c) * kMaxChunk));
            Append<std::uint32_t>(head, size << 8 | 1u);
            Append<std::uint32_t>(head, size);
        }
        Align(head, 0x10);
        Write(head.data(), head.size());
        for (std::uint32_t c = 0; c < chunks; ++c) {
            const std::size_t from = std::size_t(c) * kMaxChunk;
            const std::size_t size = std::min<std::size_t>(kMaxChunk, data.size() - from);
            Write(data.data() + from, size);
            Pad();
        }
        const std::uint64_t length = contentSize_ - resourceAt;
        hasResources_ = true;
        if (start_ + contentSize_ + 0x10 > 0xFFFFFFF0ull) tooLarge_ = true;

        DrmLocation location{};
        location.uniqueKey = static_cast<std::int32_t>(std::uint32_t(key.type) << 24 | (key.id & 0xFFFFFF));
        location.part = 0;
        location.archiveId = static_cast<std::int16_t>(id_);
        location.offset = static_cast<std::uint32_t>(resourceAt);   // fixed up by Start() by the caller
        location.size = static_cast<std::uint32_t>(length);
        location.decompressionOffset = decompressionOffset_;
        decompressionOffset_ = (decompressionOffset_ + static_cast<std::uint32_t>(data.size()) + 0xF) & ~0xFu;
        return location;
    }

    void AddFile(const std::string& path, std::vector<std::uint8_t> data) {
        files_.push_back({GameArchive::PathHash(path), std::move(data)});
    }
    // A file known only by its name hash (e.g. a sound stream whose name the game builds at run time).
    void AddFileHash(std::uint32_t hash, std::vector<std::uint8_t> data) { files_.push_back({hash, std::move(data)}); }

    // The last NEXT block, the files after the resources (each at a 16-byte boundary), then the header and the file
    // table at the start.
    bool Save(std::string& error) {
        // The game binary-searches an archive's file table by name hash (the retail tables are sorted): unsorted, a
        // second outfit's .drm is not found ("Can't open file pcx64-w\\<object>.drm").
        std::stable_sort(files_.begin(), files_.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        const std::size_t fileCount = files_.size();
        if (0x14 + 0x20 + fileCount * sizeof(FileEntry) > start_) {
            error = "the archive's file table is too small";
            return false;
        }
        if (hasResources_) WriteNext(0);
        std::vector<std::uint8_t> head;
        head.reserve(start_);
        Append<std::uint32_t>(head, kTigerMagic);
        Append<std::int32_t>(head, kTigerVersion);
        Append<std::int32_t>(head, 1);
        Append<std::int32_t>(head, static_cast<std::int32_t>(fileCount));
        Append<std::int32_t>(head, id_);
        const char platform[0x20] = "pcx64-w";
        head.insert(head.end(), platform, platform + sizeof(platform));
        const std::size_t tableAt = head.size();
        head.resize(start_, 0);
        for (std::size_t i = 0; i < fileCount; ++i) {
            Pad();
            FileEntry entry{};
            entry.nameHash = files_[i].first;
            entry.locale = kGenericLocale;
            entry.size = static_cast<std::uint32_t>(files_[i].second.size());
            entry.compressedSize = 0;
            entry.part = 0;
            entry.archiveId = static_cast<std::int16_t>(id_);
            entry.offset = static_cast<std::uint32_t>(start_ + contentSize_);
            std::memcpy(head.data() + tableAt + i * sizeof(FileEntry), &entry, sizeof(entry));
            Write(files_[i].second.data(), files_[i].second.size());
        }
        if (tooLarge_ || start_ + contentSize_ > 0xFFFFFFF0ull) {
            error = "the archive would be larger than 4 GB";
            return false;
        }
        file_.seekp(0);
        file_.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
        file_.close();
        if (!file_) {
            error = "write failed: " + path_.string();
            std::error_code ec;
            fs::remove(path_, ec);
            return false;
        }
        return true;
    }

private:
    void Write(const void* data, std::size_t size) {
        file_.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        contentSize_ += size;
    }
    // zeros to the next 16-byte boundary (the archive's start is one)
    void Pad() {
        static const char zeros[16] = {};
        if (const std::size_t pad = static_cast<std::size_t>((0x10 - (contentSize_ & 0xF)) & 0xF)) Write(zeros, pad);
    }
    void WriteNext(std::uint32_t next) {
        std::uint8_t block[16] = {};
        std::memcpy(block, &kNextMagic, 4);
        std::memcpy(block + 4, &next, 4);
        Write(block, sizeof(block));
    }

    int id_;
    fs::path path_;
    std::ofstream file_;
    std::size_t start_ = 0;
    std::uint64_t contentSize_ = 0;   // bytes written after start_
    std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> files_;   // name hash, contents
    bool hasResources_ = false;
    bool tooLarge_ = false;
    std::uint32_t decompressionOffset_ = 0;
};

// ---- fingerprint ------------------------------------------------------------------------------------------------------

struct Fingerprint {
    std::uint64_t value = 0xCBF29CE484222325ull;
    void Add(const void* data, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < size; ++i) value = (value ^ p[i]) * 0x100000001B3ull;
    }
    void Add(const std::string& s) { Add(s.data(), s.size() + 1); }
    template <typename T> void AddValue(T v) { Add(&v, sizeof(v)); }
    std::string Hex() const {
        char text[20];
        std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
        return text;
    }
};

std::vector<BuiltObject> ReadNfoObjects(const fs::path& nfo) {
    std::vector<BuiltObject> objects;
    std::ifstream in(nfo);
    std::string line;
    const std::string tag = "custom rottrframework-object:";
    while (std::getline(in, line)) {
        if (line.rfind(tag, 0) != 0) continue;
        const std::size_t comma = line.find(',', tag.size());
        if (comma == std::string::npos) continue;
        objects.push_back({line.substr(comma + 1),
                           static_cast<std::uint32_t>(std::strtoul(line.c_str() + tag.size(), nullptr, 10))});
    }
    return objects;
}

std::vector<RenamedResource> ReadNfoRenamed(const fs::path& nfo, const char* which = "renamed") {
    std::vector<RenamedResource> renamed;
    std::ifstream in(nfo);
    std::string line;
    const std::string tag = std::string("custom rottrframework-") + which + ":";
    while (std::getline(in, line)) {
        if (line.rfind(tag, 0) != 0) continue;
        unsigned type = 0, oldId = 0, newId = 0;
        if (std::sscanf(line.c_str() + tag.size(), "%u,%u,%u", &type, &oldId, &newId) == 3 && type < 32)
            renamed.push_back({static_cast<std::uint8_t>(type), oldId, newId});
    }
    return renamed;
}

std::vector<BuiltPicture> ReadNfoPictures(const fs::path& nfo) {
    std::vector<BuiltPicture> pictures;
    std::ifstream in(nfo);
    std::string line;
    const std::string tag = "custom rottrframework-picture:";
    while (std::getline(in, line)) {
        if (line.rfind(tag, 0) != 0) continue;
        const std::size_t a = line.find('|', tag.size());
        const std::size_t b = a == std::string::npos ? a : line.find('|', a + 1);
        if (b == std::string::npos) continue;
        // (imageRes[,texture,width,height]|key|path)
        char* end = nullptr;
        BuiltPicture p{line.substr(a + 1, b - a - 1), static_cast<std::uint32_t>(std::strtoul(line.c_str() + tag.size(), &end, 10)),
                       line.substr(b + 1)};
        if (end && *end == ',') {
            p.texture = static_cast<std::uint32_t>(std::strtoul(end + 1, &end, 10));
            if (end && *end == ',') p.width = static_cast<std::uint16_t>(std::strtoul(end + 1, &end, 10));
            if (end && *end == ',') p.height = static_cast<std::uint16_t>(std::strtoul(end + 1, &end, 10));
        }
        pictures.push_back(std::move(p));
    }
    return pictures;
}

std::string ReadNfoFingerprint(const fs::path& nfo) {
    std::ifstream in(nfo);
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("custom rottrframework:", 0) == 0) return line.substr(22);
    return {};
}

// Outfit .drm names of the retail outfits (TrRebootModTools' outfit reference): resources the supplied ones refer to
// are looked up there when the base outfit does not have them.
constexpr const char* kRetailOutfitObjects[] = {
    "laracroft_armor_mongolsbane", "laracroft_huntress_bloodred", "laracroft_jacket_light_battleworn",
    "laracroft_long_sleeve_blue", "laracroft_classic02", "laracroft_classic05", "laracroft_classic04",
    "laracroft_classic01", "laracroft_classic03", "laracroft_infiltrator_stalker", "laracroft_tanktop_aqua",
    "laracroft", "laracroft_snow_gear", "laracroft_long_sleeve", "laracroft_armor_vanguard", "laracroft_huntress",
    "laracroft_armor", "laracroft_infiltrator_deceiver", "laracroft_hoodie", "laracroft_huntress_thebeast",
    "laracroft_endurance", "laracroft_classic06", "laracroft_jacket_light", "laracroft_long_sleeve_rust",
    "laracroft_armor_demonseed", "laracroft_infiltrator", "laracroft_infiltrator_drifter",
    "laracroft_huntress_snowwhite", "laracroft_spiritweaver", "laracroft_colddarkness", "laracroft_pringles",
    "laracroft_snow_gear_whiteout", "laracroft_witch",
};

std::string DrmPath(const std::string& object) { return "pcx64-w\\" + Lower(object) + ".drm"; }

// The "source_object" of the folder's export.json (lower case; empty without one).
std::string ExportSource(const fs::path& folder) {
    if (folder.empty()) return {};
    std::ifstream in(folder / "export.json", std::ios::binary);
    if (!in) return {};
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto j = nlohmann::json::parse(text, nullptr, false, true);
    if (j.is_discarded() || !j.is_object()) return {};
    auto it = j.find("source_object");
    if (it == j.end() || !it->is_string()) return {};
    std::string name = Lower(it->get<std::string>());
    const bool safe = !name.empty() && name.size() < 64 &&
                      std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
    return safe ? name : std::string();
}

// ---- IDs with fixed-size tables -------------------------------------------------------------------------------------
// Most resource types are found through hash maps; new resources get IDs just above the highest of their type in
// any archive (as mods do; IDs keep 24 bits in a .drm key). Two types are not:
//  - DTP data: DTPDataSection keeps one pointer per ID in a static array (ROTTR.exe+0x1AA0B20) of 0x113000 entries
//    and stops the game with "Out of range DTPData ID!" for anything above;
//  - animations: ANITRACKER keeps 0x7D00 entries (no check at all).
// New resources of these types get IDs no archive uses: every .drm of the game is scanned once (the result is kept
// in ROTTRFramework\outfits\_ids.cache until the archives change). Resource IDs are per type, not global.
constexpr std::uint32_t kDtpIdLimit = 0x113000u;
constexpr std::uint32_t kAnimationIdLimit = 0x7D00u;
constexpr std::uint32_t kAnimationIdTop = 30999u;   // 31000.. are the framework's own clips (Acro, Grapple, Brawl)
constexpr std::uint32_t kIdCacheMagic = 0x44494652u;   // "RFID"
constexpr std::uint32_t kIdScanVersion = 3;   // 2: highest ID per type; 3: every ID of every type

struct UsedIds {
    std::vector<bool> dtp = std::vector<bool>(kDtpIdLimit, false);
    std::vector<bool> animation = std::vector<bool>(kAnimationIdLimit, false);
    std::vector<std::uint32_t> other[32];   // every other type: the IDs in use, sorted
    std::uint32_t maxId[32] = {};   // highest ID per resource type in any .drm
    std::size_t drms = 0;
};

std::uint64_t ArchiveSetStamp(const ArchiveSet& set) {
    Fingerprint stamp;
    stamp.AddValue(kIdScanVersion);
    for (const auto& archive : set.archives) {
        stamp.Add(archive.name);
        stamp.AddValue(archive.id);
        stamp.AddValue(archive.version);
        stamp.AddValue(archive.files.size());
        std::error_code ec;
        stamp.AddValue(static_cast<std::uintmax_t>(fs::file_size(archive.basePath, ec)));
    }
    return stamp.value;
}

bool LoadIdCache(const fs::path& path, std::uint64_t stamp, UsedIds& ids) {
    std::ifstream in(path, std::ios::binary);
    std::uint32_t magic = 0;
    std::uint64_t saved = 0;
    in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    in.read(reinterpret_cast<char*>(&saved), sizeof(saved));
    if (!in || magic != kIdCacheMagic || saved != stamp) return false;
    auto readBits = [&](std::vector<bool>& bits) {
        std::vector<std::uint8_t> bytes((bits.size() + 7) / 8);
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        for (std::size_t i = 0; i < bits.size(); ++i) bits[i] = (bytes[i / 8] >> (i % 8)) & 1;
    };
    readBits(ids.dtp);
    readBits(ids.animation);
    in.read(reinterpret_cast<char*>(ids.maxId), sizeof(ids.maxId));
    for (auto& list : ids.other) {
        std::uint32_t count = 0;
        in.read(reinterpret_cast<char*>(&count), sizeof(count));
        if (!in || count > 0x1000000u) return false;
        list.resize(count);
        if (count) in.read(reinterpret_cast<char*>(list.data()), std::streamsize(count) * 4);
    }
    return static_cast<bool>(in);
}

void SaveIdCache(const fs::path& path, std::uint64_t stamp, const UsedIds& ids) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(&kIdCacheMagic), sizeof(kIdCacheMagic));
    out.write(reinterpret_cast<const char*>(&stamp), sizeof(stamp));
    auto writeBits = [&](const std::vector<bool>& bits) {
        std::vector<std::uint8_t> bytes((bits.size() + 7) / 8, 0);
        for (std::size_t i = 0; i < bits.size(); ++i)
            if (bits[i]) bytes[i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    };
    writeBits(ids.dtp);
    writeBits(ids.animation);
    out.write(reinterpret_cast<const char*>(ids.maxId), sizeof(ids.maxId));
    for (const auto& list : ids.other) {
        const auto count = static_cast<std::uint32_t>(list.size());
        out.write(reinterpret_cast<const char*>(&count), sizeof(count));
        if (count) out.write(reinterpret_cast<const char*>(list.data()), std::streamsize(count) * 4);
    }
}

// Every DTP and animation ID any .drm in the archives (all versions and languages) has.
bool ScanUsedIds(ArchiveSet& set, const fs::path& cachePath, UsedIds& ids, const LogFn& log) {
    const std::uint64_t stamp = ArchiveSetStamp(set);
    if (LoadIdCache(cachePath, stamp, ids)) return true;
    const auto start = std::chrono::steady_clock::now();
    std::size_t files = 0;
    std::vector<std::uint8_t> head;
    for (const auto& archive : set.archives)
        for (const auto& file : archive.files) {
            ++files;
            const Location location{file.archiveId, file.part, file.offset,
                                    file.compressedSize ? file.compressedSize : file.size};
            if (file.size < 0x20) continue;
            std::size_t need = 0x20 + 0x14 * 256;
            if (!set.ReadPrefix(location, std::min<std::size_t>(need, file.size), head) || head.size() < 0x20 ||
                Get<std::int32_t>(head, 0) != kDrmVersion)
                continue;
            const std::int32_t count = Get<std::int32_t>(head, 24);
            if (count <= 0) continue;
            need = 0x20 + 0x14 * std::size_t(count);
            if (need > file.size) continue;
            if (head.size() < need && (!set.ReadPrefix(location, need, head) || head.size() < need)) continue;
            ++ids.drms;
            for (std::int32_t i = 0; i < count; ++i) {
                DrmIdentification id;
                std::memcpy(&id, head.data() + 0x20 + 0x14 * std::size_t(i), sizeof(id));
                if (id.id < 0) continue;
                const auto value = static_cast<std::uint32_t>(id.id);
                if (id.type < 32) ids.maxId[id.type] = std::max(ids.maxId[id.type], value);
                if (id.type == kTypeDtp && value < kDtpIdLimit) ids.dtp[value] = true;
                else if (id.type == kTypeAnimation && value < kAnimationIdLimit) ids.animation[value] = true;
                else if (id.type < 32 && id.type != kTypeDtp && id.type != kTypeAnimation)
                    ids.other[id.type].push_back(value);
            }
        }
    for (auto& list : ids.other) {
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    if (!ids.drms) return false;
    log("scanned " + std::to_string(ids.drms) + " .drm files (of " + std::to_string(files) +
        " archive files) for the resource IDs in use (" + std::to_string(ms.count()) +
        " ms; kept until the archives change)");
    SaveIdCache(cachePath, stamp, ids);
    return true;
}

// Hands out the new IDs of one build.
class IdAllocator {
public:
    explicit IdAllocator(const UsedIds& used) : used_(used) {}

    // A supplied resource with an ID no .drm in the archives uses for its type (a mod's own new ID, wherever it is in
    // the range) keeps it: the mod author picked it, and TrRebootModTools' mod manager installs it so. An ID the game
    // uses for another resource of the type has to change: the game keeps one resource per type and ID, so the two
    // versions would mix whenever both outfits are loaded (during every swap). Returns false when the ID must change.
    bool Keep(std::uint8_t type, std::uint32_t id) {
        if (type >= 32 || id == 0 || id > 0xFFFFFFu || !given_[type].insert(id).second) return false;
        bool unused = false;
        if (type == kTypeDtp) unused = id < kDtpIdLimit && !used_.dtp[id];
        else if (type == kTypeAnimation) unused = id < kAnimationIdLimit && !used_.animation[id];
        else unused = !std::binary_search(used_.other[type].begin(), used_.other[type].end(), id);
        if (!unused) {
            given_[type].erase(id);
            return false;
        }
        if (type != kTypeDtp && type != kTypeAnimation) keptMax_ = std::max(keptMax_, id);
        return true;
    }

    bool Next(std::uint8_t type, std::uint32_t& id) {
        if (type >= 32) return false;
        bool ok = false;
        if (type == kTypeDtp) ok = FromTop(used_.dtp, dtpNext_, id);
        else if (type == kTypeAnimation) ok = FromTop(used_.animation, animationNext_, id);
        else {
            // Other types (found through hash maps): one counter above every ID of any type in the archives and
            // above the kept mod IDs, where mods put their new resources too (24 bits: the .drm key keeps no more).
            if (!shared_) {
                std::uint32_t top = keptMax_;
                for (std::uint32_t max : used_.maxId) top = std::max(top, max);
                shared_ = ((top + 0xFFFFu) & ~0xFFFFu) + 0x10000u;
            }
            while (shared_ <= 0xFFFFFFu && given_[type].count(shared_)) ++shared_;
            ok = shared_ <= 0xFFFFFFu;
            if (ok) id = shared_++;
        }
        if (ok) given_[type].insert(id);
        return ok;
    }

private:
    bool FromTop(const std::vector<bool>& usedIds, std::uint32_t& next, std::uint32_t& id) {
        const std::uint8_t type = &usedIds == &used_.dtp ? kTypeDtp : kTypeAnimation;
        while (next > 0 && (usedIds[next] || given_[type].count(next))) --next;
        if (next == 0) return false;
        id = next--;
        return true;
    }
    const UsedIds& used_;
    std::uint32_t dtpNext_ = kDtpIdLimit - 1;
    std::uint32_t animationNext_ = kAnimationIdTop;
    std::uint32_t shared_ = 0;
    std::uint32_t keptMax_ = 0;
    std::set<std::uint32_t> given_[32];
};

// ---- zonelist.ids: object names and their zone (object) IDs ---------------------------------------------------------
constexpr std::uint32_t kZoneIdMargin = 32;

struct UserResource {
    fs::path path;
    Key key;
};

struct BuiltOutfit {
    std::string object;
    std::uint32_t zoneId = 0;
    Drm drm;
    std::vector<RenamedResource> renamed, copied;
};

// The .drm entries of the given resources, from the .drm files the game uses (the current copy of each; generic
// locale). When several refer to a resource, the entry whose data is in the highest-priority archive wins: patches
// re-point resources at newer data, and old data (an older shader library) does not match the game any more.
std::map<Key, std::pair<DrmIdentification, DrmLocation>> FindInArchives(ArchiveSet& set, const std::set<Key>& keys) {
    std::map<Key, std::pair<DrmIdentification, DrmLocation>> result;
    std::vector<const Archive*> order;
    for (const auto& archive : set.archives) order.push_back(&archive);
    std::sort(order.begin(), order.end(), [](const Archive* a, const Archive* b) { return a->rank > b->rank; });
    std::vector<std::uint8_t> head;
    // A .drm only refers to data in its own archive or older ones, so once every resource is found in data at least
    // as new as the archives left to scan, nothing better can come.
    auto settled = [&](int rank) {
        if (result.size() != keys.size()) return false;
        for (const auto& [key, entry] : result)
            if (set.RankOf(entry.second.archiveId) < rank) return false;
        return true;
    };
    for (const Archive* archive : order) {
        if (settled(archive->rank)) break;
        for (const auto& file : archive->files) {
            if (file.size < 0x20 || !set.IsCurrent(*archive, file)) continue;
            const Location location{file.archiveId, file.part, file.offset,
                                    file.compressedSize ? file.compressedSize : file.size};
            if (!set.ReadPrefix(location, 0x20, head) || head.size() < 0x20 || Get<std::int32_t>(head, 0) != kDrmVersion)
                continue;
            const std::int32_t count = Get<std::int32_t>(head, 24);
            const std::int32_t includes = Get<std::int32_t>(head, 4), dependencies = Get<std::int32_t>(head, 8);
            if (count <= 0 || includes < 0 || dependencies < 0) continue;
            const std::size_t locationsAt = 0x20 + 0x14 * std::size_t(count) + std::size_t(includes) + std::size_t(dependencies);
            const std::size_t need = locationsAt + 0x18 * std::size_t(count);
            if (need > file.size || !set.ReadPrefix(location, need, head) || head.size() < need) continue;
            for (std::int32_t i = 0; i < count; ++i) {
                DrmIdentification id;
                std::memcpy(&id, head.data() + 0x20 + 0x14 * std::size_t(i), sizeof(id));
                if (id.id < 0 || id.locale != kGenericLocale) continue;
                const Key key{id.type, static_cast<std::uint32_t>(id.id)};
                if (!keys.count(key)) continue;
                DrmLocation where;
                std::memcpy(&where, head.data() + locationsAt + 0x18 * std::size_t(i), sizeof(where));
                auto found = result.find(key);
                if (found == result.end() ||
                    set.RankOf(where.archiveId) > set.RankOf(found->second.second.archiveId))
                    result[key] = {id, where};
            }
        }
    }
    return result;
}

// What one build reads again and again, read once (framework 0.9.403). Every outfit reads its base's .drm and the
// base resources it scans (thousands for laracroft, each decompressed), the retail outfits' .drm files, and looks for
// the resources no outfit's .drm has in every .drm of the game (FindInArchives: the headers of all of them; the 6
// that laracroft's copies refer to are in none, so every copy scanned them all again). 107 outfits (101 copies of
// laracroft) took 163 s, most of it in these reads; the answers do not change within a build.
class BuildCache {
public:
    // A .drm file of the archives, parsed (nullptr: not there or unreadable).
    const Drm* ReadDrm(ArchiveSet& set, const std::string& path) {
        auto it = drms_.find(path);
        if (it == drms_.end()) {
            std::vector<std::uint8_t> bytes;
            std::optional<Drm> drm;
            if (set.ReadFile(path, bytes)) {
                Drm parsed;
                if (parsed.Parse(bytes)) drm = std::move(parsed);
            }
            ++drmReads;
            it = drms_.emplace(path, std::move(drm)).first;
        }
        return it->second ? &*it->second : nullptr;
    }

    // A resource blob, as ArchiveSet::ReadBlob gives it (nullptr: unreadable). Kept while the kept ones stay under
    // kMaxBytes (laracroft's scanned resources: about 20 MB).
    std::shared_ptr<const std::vector<std::uint8_t>> ReadBlob(ArchiveSet& set, const Location& where, std::uint32_t expected) {
        const BlobKey key{where.archiveId, where.archiveIndex, where.part, where.offset, where.size, expected};
        if (auto it = blobs_.find(key); it != blobs_.end()) return it->second;
        auto data = std::make_shared<std::vector<std::uint8_t>>();
        std::shared_ptr<const std::vector<std::uint8_t>> result;
        if (set.ReadBlob(where, *data, expected)) result = std::move(data);
        ++blobReads;
        const std::size_t size = result ? result->size() : 0;
        if (blobBytes_ + size <= kMaxBytes) {
            blobBytes_ += size;
            blobs_.emplace(key, result);
        }
        return result;
    }

    // FindInArchives, each resource looked for once (found or not). A resource's answer does not depend on the others
    // asked with it: the archives are read from the highest priority down, and FindInArchives only stops early once
    // nothing in the archives left can hold a better entry.
    std::map<Key, std::pair<DrmIdentification, DrmLocation>> Find(ArchiveSet& set, const std::set<Key>& keys) {
        std::set<Key> unknown;
        for (const auto& key : keys)
            if (!located_.count(key)) unknown.insert(key);
        if (!unknown.empty()) {
            const auto found = FindInArchives(set, unknown);
            ++archiveSearches;
            for (const auto& key : unknown) {
                auto it = found.find(key);
                located_[key] = it == found.end() ? std::nullopt
                                                  : std::optional<std::pair<DrmIdentification, DrmLocation>>(it->second);
            }
        }
        std::map<Key, std::pair<DrmIdentification, DrmLocation>> result;
        for (const auto& key : keys)
            if (const auto& entry = located_[key]) result.emplace(key, *entry);
        return result;
    }

    std::size_t drmReads = 0, blobReads = 0, archiveSearches = 0;

private:
    static constexpr std::size_t kMaxBytes = std::size_t(512) << 20;
    struct BlobKey {
        int archiveId, archiveIndex, part;
        std::uint32_t offset, size, expected;
        bool operator<(const BlobKey& o) const {
            return std::tie(archiveId, archiveIndex, part, offset, size, expected) <
                   std::tie(o.archiveId, o.archiveIndex, o.part, o.offset, o.size, o.expected);
        }
    };
    std::map<std::string, std::optional<Drm>> drms_;
    std::map<BlobKey, std::shared_ptr<const std::vector<std::uint8_t>>> blobs_;
    std::size_t blobBytes_ = 0;
    std::map<Key, std::optional<std::pair<DrmIdentification, DrmLocation>>> located_;
};

// One outfit: its new .drm, with every new resource stored in the writer.
bool BuildOutfit(ArchiveSet& set, BuildCache& cache, ArchiveWriter& writer, const OutfitSpec& spec, IdAllocator& ids,
                 const ZoneTable& zones, std::uint32_t zoneId, BuiltOutfit& built, const LogFn& log) {
    auto say = [&](const std::string& text) { log(spec.key + ": " + text); };
    // A new model gets a new object: never replace one of the game's (or another mod's) objects, but for a mod's
    // replacement of a retail outfit, which overrides that outfit's own object (its .drm, under its zone ID).
    if (!spec.replacesObject && set.HasFile(DrmPath(spec.objectName))) {
        say("Object=" + spec.objectName + " already exists in the game archives; a new model needs a new object "
            "name (for example laracroft_" + spec.key + ")");
        return false;
    }
    // Supplied resources.
    std::vector<UserResource> users;
    std::error_code ec;
    const std::set<std::string> excluded(spec.exclude.begin(), spec.exclude.end());
    std::size_t left = 0;
    if (!spec.folder.empty())
        for (auto it = fs::recursive_directory_iterator(spec.folder, ec); it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            Key key;
            if (!ParseResourceFileName(it->path(), key)) continue;
            if (excluded.count(ExcludeKey(spec.folder, it->path()))) {
                ++left;
                continue;
            }
            users.push_back({it->path(), key});
        }
    if (left) say(std::to_string(left) + " file(s) left out (\"exclude\" in mod.json)");
    // the mod's shared files it uses (after the folder's: one of its own for the same resource wins)
    std::size_t sharedUsed = 0;
    for (const auto& path : spec.shared) {
        Key key;
        if (!ParseResourceFileName(path, key)) continue;
        if (std::any_of(users.begin(), users.end(), [&](const UserResource& u) { return u.key == key; })) continue;
        users.push_back({path, key});
        ++sharedUsed;
    }
    if (sharedUsed) say(std::to_string(sharedUsed) + " of the mod's shared file(s) used");
    if (spec.cloneOnly && users.empty()) {
        say("a copy of " + Lower(spec.baseObjectName) + " (no model files)");
    } else if (users.empty()) {
        say("no resource files in " + spec.folder.string());
        return false;
    }

    // The outfit the files were exported from: Model=, or the outfit .drm holding most of their IDs (models count
    // most: a mesh belongs to one outfit, materials and textures are often shared).
    std::string baseName = Lower(spec.baseObjectName);
    const Drm* baseDrm = nullptr;
    // export.json (written by the Blender add-on next to the files) names the object they were exported from.
    if (const std::string source = ExportSource(spec.folder); spec.detectBase && !source.empty()) {
        if ((baseDrm = cache.ReadDrm(set, DrmPath(source)))) {
            baseName = source;
            say("built from " + baseName + " (export.json)");
        } else {
            say("export.json names " + source + ", which is not in the game archives");
        }
    }
    if (spec.detectBase && !baseDrm) {
        std::vector<std::string> candidates{baseName};
        for (const char* name : kRetailOutfitObjects) candidates.push_back(name);
        for (const auto& source : spec.sources) candidates.push_back(Lower(source));
        std::set<Key> wanted;
        for (const auto& user : users) wanted.insert(user.key);
        int bestScore = 0;
        std::set<std::string> seen;
        for (const auto& candidate : candidates) {
            if (!seen.insert(candidate).second) continue;
            const Drm* drm = cache.ReadDrm(set, DrmPath(candidate));
            if (!drm) continue;
            int score = 0;
            std::set<Key> counted;
            for (const auto& id : drm->ids) {
                const Key key{id.type, static_cast<std::uint32_t>(id.id)};
                if (wanted.count(key) && counted.insert(key).second) score += key.type == kTypeModel ? 1000 : 1;
            }
            if (score > bestScore) {
                bestScore = score;
                baseName = candidate;
                baseDrm = drm;
            }
        }
        if (bestScore > 0)
            say("built from " + baseName + " (the outfit holding the supplied resource IDs; set Model= to choose)");
    }
    if (!baseDrm && !(baseDrm = cache.ReadDrm(set, DrmPath(baseName)))) {
        say("the base outfit's .drm (" + DrmPath(baseName) + ") was not found in the game archives");
        return false;
    }
    const Drm& base = *baseDrm;

    // (v0.9.602) A game .drm that is no object (mod.json "models" naming a font library, pcx64-w\fontuniversal.drm:
    // no zone resource, its Scaleform movies found by their own DTP IDs; the Modding Suite's Fonts tab saves into one):
    // the mod's files replace its resources in place, under the same IDs, as TrRebootTools' mod manager installs them.
    // Nothing gets a new ID (no reference names the font movies to re-point), and no zone is given to it.
    if (spec.replacesObject && !zones.ids.count(baseName)) {
        built.object = Lower(spec.objectName);
        built.zoneId = 0;
        built.drm = base;
        std::size_t replaced = 0, added = 0;
        std::set<Key> seen;
        for (const auto& user : users) {
            if (!seen.insert(user.key).second) {
                say("two files for the same resource: " + user.path.filename().string() + " (the first is used)");
                continue;
            }
            std::vector<std::uint8_t> data;
            if (!ReadDisk(user.path, data)) {
                say("cannot read " + user.path.string());
                return false;
            }
            // its base entries (every locale) and their reference definitions' size: a font movie (DTP subtype 50)
            // has none, its data starts with the movie's length
            std::vector<std::size_t> entries;
            for (std::size_t i = 0; i < base.ids.size(); ++i)
                if (base.ids[i].type == user.key.type && static_cast<std::uint32_t>(base.ids[i].id) == user.key.id) entries.push_back(i);
            if (user.key.type == kTypeTexture) {
                bool srgb = false;
                std::vector<std::uint8_t> head;
                if (!entries.empty() && set.ReadBlobHead(base.LocationOf(entries.front()), head) && head.size() >= 8)
                    srgb = IsSrgb(Get<std::uint32_t>(head, 4));
                std::vector<std::uint8_t> converted;
                std::string error;
                if (!ConvertDds(data, srgb, converted, error)) {
                    say(user.path.filename().string() + ": " + error);
                    return false;
                }
                data = std::move(converted);
            }
            std::uint32_t refSize = 0;
            if (!entries.empty()) {
                refSize = base.RefSize(entries.front());
            } else if (user.key.type != kTypeTexture && user.key.type != kTypeShaderLib && user.key.type != kTypeSoundBank) {
                const std::uint64_t size = RefDefinitionsSize(data);
                refSize = size <= data.size() ? static_cast<std::uint32_t>(size) : 0;
            }
            if (refSize > data.size()) {
                say(user.path.filename().string() + ": shorter than the reference definitions of the resource it replaces");
                return false;
            }
            const DrmLocation location = writer.AddResource(data, user.key);
            const std::uint32_t bodySize = static_cast<std::uint32_t>(data.size()) - refSize;
            for (const std::size_t i : entries) {
                DrmIdentification& id = built.drm.ids[i];
                id.bodySize = bodySize;
                id.subTypeAndRefSize = (id.subTypeAndRefSize & 0xFFu) | (refSize << 8);
                built.drm.locations[i] = location;
            }
            if (!entries.empty()) {
                ++replaced;
                continue;
            }
            DrmIdentification id{};
            id.type = user.key.type;
            id.locale = kGenericLocale;
            id.id = static_cast<std::int32_t>(user.key.id);
            id.bodySize = bodySize;
            id.subTypeAndRefSize = refSize << 8;
            built.drm.ids.push_back(id);
            built.drm.locations.push_back(location);
            ++added;
        }
        // (the Modding Suite looks for this text in the .asi to tell a framework that builds a .drm that is no object)
        say("not an object of the game: " + std::to_string(replaced) + " of " + DrmPath(baseName) +
            "'s resources replaced in place (same IDs), " + std::to_string(added) + " added");
        return true;
    }

    std::map<Key, std::size_t> baseIndex;
    for (std::size_t i = 0; i < base.ids.size(); ++i) {
        const Key key{base.ids[i].type, static_cast<std::uint32_t>(base.ids[i].id)};
        if (!baseIndex.count(key) || base.ids[i].locale == kGenericLocale) baseIndex[key] = i;
    }

    bool outOfIds = false;
    auto freshId = [&](std::uint8_t type) {
        std::uint32_t id = 0;
        if (!ids.Next(type, id)) outOfIds = true;
        return id;
    };
    std::map<Key, std::uint32_t> remap;
    std::map<Key, std::size_t> userIndex;
    for (std::size_t i = 0; i < users.size(); ++i) {
        if (userIndex.count(users[i].key)) {
            say("two files for the same resource: " + users[i].path.filename().string() + " (the first is used)");
            continue;
        }
        userIndex[users[i].key] = i;
        // A mod's own new IDs stay (as the mod manager installs them); IDs the game uses (replaced resources) change.
        if (ids.Keep(users[i].key.type, users[i].key.id)) remap[users[i].key] = users[i].key.id;
    }
    std::size_t kept = remap.size();
    for (const auto& [key, index] : userIndex)
        if (!remap.count(key)) remap[key] = freshId(key.type);

    // The object itself: its zone resource (type 14, ID = the object's zone ID, e.g. 77 for laracroft) becomes the
    // new object's. The game finds an object's data by its zone ID once its .drm is loaded.
    std::uint32_t baseZone = 0;
    if (auto zone = zones.ids.find(baseName); zone != zones.ids.end()) baseZone = zone->second;
    if (!baseZone || !baseIndex.count(Key{kTypeObjectRef, baseZone})) {
        say("the zone resource of " + baseName + " was not found in its .drm");
        return false;
    }
    remap[Key{kTypeObjectRef, baseZone}] = zoneId;

    // Base resources that refer to a replaced one are replaced too (copied with new IDs), up to the root.
    std::map<Key, std::shared_ptr<const std::vector<std::uint8_t>>> baseData;
    std::set<Key> unreadable;
    auto loadBase = [&](const Key& key) -> const std::vector<std::uint8_t>* {
        auto cached = baseData.find(key);
        if (cached != baseData.end()) return cached->second.get();
        if (unreadable.count(key)) return nullptr;
        const std::size_t i = baseIndex[key];
        auto data = cache.ReadBlob(set, base.LocationOf(i), base.TotalSize(i));
        if (!data) {
            unreadable.insert(key);
            return nullptr;
        }
        return (baseData[key] = std::move(data)).get();
    };
    // The zone resource points at the object's dtp::ZoneData (a DTP), whose first field m_zoneID is the zone ID the
    // game files the loaded object under (ObjectZoneManager::RegisterZoneData). That DTP is copied too, with
    // m_zoneID set to the new zone (patched below); otherwise loading the object stops the game.
    Key zoneDataKey{};
    if (auto* zoneResource = loadBase(Key{kTypeObjectRef, baseZone})) {
        std::vector<Ref> refs;
        const std::uint64_t refSize = RefDefinitionsSize(*zoneResource);
        if (ParseRefs(*zoneResource, refs))
            for (const auto& ref : refs)
                if (ref.type == kTypeDtp && ref.bodyAt == refSize) {
                    zoneDataKey = Key{ref.type, ref.id};
                    break;
                }
    }
    if (!zoneDataKey.id) {
        say("the object data (ZoneData) of " + baseName + " was not found");
        return false;
    }
    if (!remap.count(zoneDataKey)) remap[zoneDataKey] = freshId(kTypeDtp);

    // Models and DTPs are also named by plain ID inside DTP data, not only through references: the model
    // component's list in the ZoneData holds model IDs, and DTP item data names its DTP by ID ({dtp, item} pairs: the
    // ZoneData repeats its own ID thousands of times, the skeleton its own a few times). Left alone, those still name
    // the base outfit's DTPs, so the new skeleton's bones and the object's items resolve into the old data (Lara stays
    // in the bind pose). DTPs holding such an ID are copied too, and the IDs are patched. modelsOnly: model IDs
    // (for the start of a model's body).
    auto plainIds = [&](bool modelsOnly = false) {
        std::map<std::uint32_t, std::uint32_t> ids;
        std::set<std::uint32_t> clash;
        for (const auto& [key, id] : remap) {
            if (key.type != kTypeModel && (modelsOnly || key.type != kTypeDtp)) continue;
            if (id == key.id) continue;
            auto [it, added] = ids.emplace(key.id, id);
            if (!added && it->second != id) clash.insert(key.id);   // same number, two types: leave it alone
        }
        for (const auto id : clash) ids.erase(id);
        return ids;
    };
    std::size_t scanned = 0;
    for (bool changed = true; changed;) {
        changed = false;
        const auto models = plainIds();
        for (const auto& [key, index] : baseIndex) {
            if (remap.count(key)) continue;
            const bool refs = CanReference(key.type) && base.RefSize(index) != 0;
            const bool plain = key.type == kTypeDtp && !models.empty();
            if (!refs && !plain) continue;
            auto* data = loadBase(key);
            if (!data) continue;
            ++scanned;
            bool hit = false;
            std::vector<Ref> found;
            if (refs && ParseRefs(*data, found))
                for (const auto& ref : found)
                    if (remap.count(Key{ref.type, ref.id})) {
                        hit = true;
                        break;
                    }
            if (!hit && plain) {
                const std::uint64_t body = RefDefinitionsSize(*data);
                for (std::size_t at = static_cast<std::size_t>(body); body <= data->size() && at + 4 <= data->size() && !hit;
                     at += 4)
                    hit = models.count(Get<std::uint32_t>(*data, at)) != 0;
            }
            if (hit) {
                remap[key] = freshId(key.type);
                changed = true;
            }
        }
    }
    if (!unreadable.empty())
        say(std::to_string(unreadable.size()) + " base resource(s) could not be read (their archive is missing)");
    if (outOfIds) {
        say("no free resource IDs left");
        return false;
    }

    // Prepare every new resource: data, references pointed at the new IDs.
    struct NewResource {
        Key key;
        std::uint32_t newId;
        std::vector<std::uint8_t> data;
        std::uint32_t refSize;
        bool fromUser;
    };
    std::vector<NewResource> fresh;
    std::set<Key> wanted;   // referenced by new resources, not new themselves
    for (const auto& [key, newId] : remap) {
        NewResource resource{key, newId, {}, 0, userIndex.count(key) != 0};
        if (resource.fromUser) {
            const auto& user = users[userIndex[key]];
            std::vector<std::uint8_t> raw;
            if (!ReadDisk(user.path, raw)) {
                say("cannot read " + user.path.string());
                return false;
            }
            if (key.type == kTypeTexture) {
                bool srgb = false;
                if (baseIndex.count(key)) {
                    std::vector<std::uint8_t> head;
                    if (set.ReadBlobHead(base.LocationOf(baseIndex[key]), head) && head.size() >= 8)
                        srgb = IsSrgb(Get<std::uint32_t>(head, 4));
                }
                std::string error;
                if (!ConvertDds(raw, srgb, resource.data, error)) {
                    say(user.path.filename().string() + ": " + error);
                    return false;
                }
            } else {
                resource.data = std::move(raw);
            }
        } else {
            auto* data = loadBase(key);
            if (!data) {
                say("a base resource that must be copied could not be read");
                return false;
            }
            resource.data = *data;
        }
        if (key.type != kTypeTexture && key.type != kTypeShaderLib && key.type != kTypeSoundBank) {
            const std::uint64_t refSize = RefDefinitionsSize(resource.data);
            if (refSize > resource.data.size()) {
                say("resource " + std::to_string(key.id) + " has invalid reference definitions");
                return false;
            }
            resource.refSize = static_cast<std::uint32_t>(refSize);
            if (key == zoneDataKey) {
                if (resource.data.size() < refSize + 4 || Get<std::uint32_t>(resource.data, refSize) != baseZone) {
                    say("the object data (ZoneData, DTP " + std::to_string(key.id) + ") does not start with zone " +
                        std::to_string(baseZone));
                    return false;
                }
                Put<std::uint32_t>(resource.data, refSize, zoneId);   // m_zoneID
            }
            std::vector<Ref> refs;
            ParseRefs(resource.data, refs);
            for (const auto& ref : refs) {
                const Key target{ref.type, ref.id};
                auto mapped = remap.find(target);
                if (mapped != remap.end()) SetRefId(resource.data, ref, mapped->second);
                else if (!baseIndex.count(target)) wanted.insert(target);
            }
            // The meshrefs' models (an extra mesh added in the Suite's object panel: a model of another outfit, by ID):
            // carried like referenced resources. (A mod's own model is a supplied file: in remap, its ID patched below.)
            if (key == zoneDataKey)
                for (const Key& target : MeshRefResources(resource.data))
                    if (!remap.count(target) && !baseIndex.count(target)) wanted.insert(target);
            // Plain model and DTP IDs (not reference slots): in DTP bodies; a model's own ID at the start of its body.
            if (key.type == kTypeDtp || key.type == kTypeModel) {
                std::set<std::size_t> slots;
                for (const auto& ref : refs)
                    if (ref.bodyAt) slots.insert(ref.bodyAt & ~std::size_t(3));
                const auto models = plainIds(key.type == kTypeModel);
                const std::size_t end = key.type == kTypeDtp ? resource.data.size()
                                                             : std::min<std::size_t>(resource.data.size(), refSize + 64);
                for (std::size_t at = refSize; at + 4 <= end; at += 4) {
                    if (slots.count(at)) continue;
                    auto model = models.find(Get<std::uint32_t>(resource.data, at));
                    if (model != models.end()) Put<std::uint32_t>(resource.data, at, model->second);
                }
            }
        }
        fresh.push_back(std::move(resource));
    }

    // Resources the new ones refer to that the base outfit does not load: take their entries from other outfits
    // (Sources=, the retail outfits), then from any .drm in the archives (as TrRebootModTools' mod manager does).
    std::vector<std::pair<DrmIdentification, DrmLocation>> borrowed;
    if (!wanted.empty()) {
        std::vector<std::string> sources = spec.sources;
        for (const char* object : kRetailOutfitObjects) sources.push_back(object);
        std::vector<const Drm*> sourceDrms;
        for (const auto& source : sources) {
            if (Lower(source) == baseName) continue;
            if (const Drm* drm = cache.ReadDrm(set, DrmPath(source))) sourceDrms.push_back(drm);
        }
        std::set<Key> found, unresolved;
        std::deque<Key> queue(wanted.begin(), wanted.end());
        auto take = [&](const Key& key, const DrmIdentification& id, const DrmLocation& location) {
            found.insert(key);
            unresolved.erase(key);
            borrowed.push_back({id, location});
            const std::uint32_t refSize = id.subTypeAndRefSize >> 8;
            if (!CanReference(key.type) || !refSize) return;
            std::vector<Ref> refs;
            const Location where{location.archiveId, location.part, location.offset, location.size};
            const auto data = cache.ReadBlob(set, where, id.bodySize + refSize);
            if (data && ParseRefs(*data, refs))
                for (const auto& ref : refs) queue.push_back(Key{ref.type, ref.id});
        };
        for (int pass = 0; pass < 6; ++pass) {
            while (!queue.empty()) {
                const Key key = queue.front();
                queue.pop_front();
                if (found.count(key) || baseIndex.count(key) || remap.count(key)) continue;
                bool hit = false;
                for (const Drm* drm : sourceDrms) {
                    for (std::size_t i = 0; i < drm->ids.size() && !hit; ++i)
                        if (drm->ids[i].type == key.type && static_cast<std::uint32_t>(drm->ids[i].id) == key.id &&
                            drm->ids[i].locale == kGenericLocale) {
                            take(key, drm->ids[i], drm->locations[i]);
                            hit = true;
                        }
                    if (hit) break;
                }
                if (!hit) unresolved.insert(key);
            }
            if (unresolved.empty()) break;
            const auto located = cache.Find(set, unresolved);
            if (located.empty()) break;
            for (const auto& [key, entry] : located) take(key, entry.first, entry.second);
        }
        if (!unresolved.empty())
            say(std::to_string(unresolved.size()) + " referenced resource(s) are in no .drm of the game "
                "(fine when the game always has them loaded, e.g. shared textures)");
    }

    // The new .drm: the base entries, replaced ones pointing at the new resources, then the added ones.
    built.object = Lower(spec.objectName);
    built.zoneId = zoneId;
    built.drm = base;
    std::size_t replaced = 0, added = 0, copied = 0;
    for (auto& resource : fresh) {
        const DrmLocation location = writer.AddResource(resource.data, Key{resource.key.type, resource.newId});
        DrmIdentification id{};
        auto at = baseIndex.find(resource.key);
        if (at != baseIndex.end()) {
            id = base.ids[at->second];
        } else {
            id.type = resource.key.type;
            id.locale = kGenericLocale;
            std::uint32_t subType = 0;
            switch (resource.key.type) {
                case kTypeTexture: subType = 5; break;
                case kTypeMaterial: subType = 18; break;
                case kTypeModel: subType = 26; break;
                case kTypeObjectRef: subType = 103; break;
                default: break;
            }
            id.subTypeAndRefSize = subType << 1;
        }
        id.id = static_cast<std::int32_t>(resource.newId);
        id.bodySize = static_cast<std::uint32_t>(resource.data.size()) - resource.refSize;
        id.subTypeAndRefSize = (id.subTypeAndRefSize & 0xFFu) | (resource.refSize << 8);
        // A base entry is overwritten only by the same key or by the object's zone resource. Resources that got a new
        // ID are added next to the original, which stays: other resources (in this .drm or ones loaded with it) may
        // still refer to it, and dropping it leaves them with nothing (an empty texture slot stopped the game).
        const bool overwrite = at != baseIndex.end() &&
                               (resource.newId == resource.key.id || resource.key.type == kTypeObjectRef);
        if (at != baseIndex.end()) resource.fromUser ? ++replaced : ++copied;
        if (overwrite) {
            built.drm.ids[at->second] = id;
            built.drm.locations[at->second] = location;
        } else if (at != baseIndex.end()) {
            built.drm.ids.push_back(id);
            built.drm.locations.push_back(location);
        } else {
            built.drm.ids.push_back(id);
            built.drm.locations.push_back(location);
            ++added;
        }
    }
    for (const auto& [id, location] : borrowed) {
        built.drm.ids.push_back(id);
        built.drm.locations.push_back(location);
    }
    say(std::to_string(kept) + " supplied resource(s) keep their own (new) IDs, " +
        std::to_string(userIndex.size() - kept) + " with IDs the game uses for other resources get new ones");
    {
        static const char* const kTypeNames[32] = {nullptr, nullptr, "animation", nullptr, "PSD", "texture", "sound",
                                                   "DTP", "script", "shader library", "material", "object data",
                                                   "model", "collision", "object", "animation library"};
        std::map<std::uint8_t, std::string> lists;
        for (const auto& [key, index] : userIndex) {
            const std::uint32_t newId = remap[key];
            if (newId == key.id) continue;
            built.renamed.push_back({key.type, key.id, newId});
            std::string& list = lists[key.type];
            list += (list.empty() ? "" : ", ") + std::to_string(key.id) + " -> " + std::to_string(newId);
        }
        // (the base's resources copied with new IDs: the live link (suite_link.cpp) edits them under the game's ID)
        for (const auto& [key, newId] : remap)
            if (!userIndex.count(key) && newId != key.id && key.type != kTypeObjectRef) built.copied.push_back({key.type, key.id, newId});
        for (const auto& [type, list] : lists)
            say("new IDs for " + std::string(type < 32 && kTypeNames[type] ? kTypeNames[type] : "type " + std::to_string(type)) +
                "s: " + list);
    }
    say(std::to_string(users.size()) + " file(s): " + std::to_string(replaced) + " replace base resources (added beside them), " +
        std::to_string(added) + " added; " + std::to_string(copied) + " base resource(s) copied to refer to them (" +
        std::to_string(scanned) + " scanned); " + std::to_string(borrowed.size()) + " taken from other outfits");
    return true;
}
// ---- campsite pictures ----------------------------------------------------------------------------------------------
// A picture is an image .drm under pcx64-w\design\image resources\: a DTP (m_imageRes of the outfit's
// dtp::ImagePath) and the texture it refers to (128x256 BC3 sRGB for the outfit paperdolls). A new picture is a copy
// of a retail outfit's image .drm with new IDs and the supplied pixels.
constexpr const char* kPictureFolder = "scaleform\\sharedtextures\\basecamp\\TRX_basecamp\\Paperdolls\\Outfits\\";
constexpr const char* kTemplatePicture = "huntress";   // Huntress_Base's picture

std::string ImageDrmPath(const std::string& imagePath) {
    return "pcx64-w\\design\\image resources\\" + Lower(imagePath) + ".drm";
}

struct BuiltPictureDrm {
    BuiltPicture picture;
    Drm drm;
};

// Past this many pictures, a picture wider than kManyPicturesWidth is halved down to it (a BC3, one level): the
// campsite's Outfits tab loads every listed outfit's picture at once (UiMenuStateCampsiteGear::LoadOutfitImages), and
// the Suite's 4x / 8x pictures (512x1024: 0.7 MB with their levels; 1024x2048) add up; at 2x (256x512) they are still
// sharper than the game's 128x256, which is the size they are shown at.
constexpr std::size_t kManyPictures = 64;
constexpr std::uint32_t kManyPicturesWidth = 256;

// converted: ConvertDds's (a 28-byte header: +4 format, +8 data size, +16 width, +18 height, +23 levels; the data).
// The first level halved (2x2 averages, in linear light for an sRGB format) until it is maxWidth wide or less, as a
// BC3 (sRGB if it was). false: left as it is (narrow enough, or a format not read here).
bool ShrinkPicture(std::vector<std::uint8_t>& converted, std::uint32_t maxWidth, std::string& note) {
    if (converted.size() < 28) return false;
    const std::uint32_t format = Get<std::uint32_t>(converted, 4);
    int width = Get<std::uint16_t>(converted, 16), height = Get<std::uint16_t>(converted, 18);
    if (static_cast<std::uint32_t>(width) <= maxWidth) return false;
    std::vector<unsigned char> rgba;
    if (!ImageLoad::DecodeTexture(format, converted.data() + 28, converted.size() - 28, width, height, rgba)) {
        note = "format " + std::to_string(format) + " is not read here: kept at " + std::to_string(width) + "x" +
               std::to_string(height);
        return false;
    }
    const bool srgb = format == 72 || format == 75 || format == 78 || format == 99 || format == 29 || format == 91;
    float toLinear[256];
    for (int i = 0; i < 256; ++i) {
        const float c = i / 255.0f;
        toLinear[i] = srgb ? (c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f)) : c;
    }
    auto fromLinear = [&](float v) -> unsigned char {
        v = std::clamp(v, 0.0f, 1.0f);
        if (srgb) v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        return static_cast<unsigned char>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    const int fromWidth = width, fromHeight = height;
    while (static_cast<std::uint32_t>(width) > maxWidth && width >= 8 && height >= 8) {
        const int w = width / 2, h = height / 2;
        std::vector<unsigned char> half(static_cast<std::size_t>(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const unsigned char* p[4] = {
                    &rgba[(static_cast<std::size_t>(2 * y) * width + 2 * x) * 4],
                    &rgba[(static_cast<std::size_t>(2 * y) * width + 2 * x + 1) * 4],
                    &rgba[(static_cast<std::size_t>(2 * y + 1) * width + 2 * x) * 4],
                    &rgba[(static_cast<std::size_t>(2 * y + 1) * width + 2 * x + 1) * 4]};
                unsigned char* q = &half[(static_cast<std::size_t>(y) * w + x) * 4];
                // (colours weighted by their alpha: a transparent pixel's colour does not bleed into the edge)
                float a = 0, rgb[3] = {0, 0, 0};
                for (const auto* px : p) {
                    const float pa = px[3] / 255.0f;
                    a += pa;
                    for (int c = 0; c < 3; ++c) rgb[c] += toLinear[px[c]] * pa;
                }
                for (int c = 0; c < 3; ++c) q[c] = a > 0 ? fromLinear(rgb[c] / a) : 0;
                q[3] = static_cast<unsigned char>(std::lround(a / 4.0f * 255.0f));
            }
        rgba.swap(half);
        width = w;
        height = h;
    }
    std::vector<std::uint8_t> out(converted.begin(), converted.begin() + 28);
    out.resize(28 + ImageLoad::Bc3Size(width, height));
    ImageLoad::CompressBc3(rgba.data(), width, height, out.data() + 28);
    Put<std::uint32_t>(out, 4, srgb ? 78u : 77u);
    Put<std::uint32_t>(out, 8, static_cast<std::uint32_t>(out.size() - 28));
    Put<std::uint16_t>(out, 16, static_cast<std::uint16_t>(width));
    Put<std::uint16_t>(out, 18, static_cast<std::uint16_t>(height));
    out[23] = 1;
    converted = std::move(out);
    note = std::to_string(fromWidth) + "x" + std::to_string(fromHeight) + " stored at " + std::to_string(width) + "x" +
           std::to_string(height) + " (more than " + std::to_string(kManyPictures) + " pictures)";
    return true;
}

bool BuildPicture(ArchiveSet& set, ArchiveWriter& writer, const OutfitSpec& spec, IdAllocator& ids,
                  BuiltPictureDrm& out, const LogFn& log, std::uint32_t maxWidth) {
    auto say = [&](const std::string& text) { log(spec.key + ": picture: " + text); };
    std::vector<std::uint8_t> dds;
    if (!ReadDisk(spec.picture, dds)) {
        say("cannot read " + spec.picture.string());
        return false;
    }
    std::vector<std::uint8_t> bytes;
    Drm base;
    const std::string templatePath = ImageDrmPath(std::string(kPictureFolder) + kTemplatePicture);
    if (!set.ReadFile(templatePath, bytes) || !base.Parse(bytes)) {
        say(templatePath + " was not found in the game archives");
        return false;
    }
    int textureIndex = -1, dtpIndex = -1;
    for (std::size_t i = 0; i < base.ids.size(); ++i) {
        if (base.ids[i].locale != kGenericLocale) continue;
        if (base.ids[i].type == kTypeTexture && textureIndex < 0) textureIndex = static_cast<int>(i);
        if (base.ids[i].type == kTypeDtp && dtpIndex < 0) dtpIndex = static_cast<int>(i);
    }
    std::vector<std::uint8_t> texture, dtp;
    if (textureIndex < 0 || dtpIndex < 0 ||
        !set.ReadBlob(base.LocationOf(std::size_t(textureIndex)), texture, base.TotalSize(std::size_t(textureIndex))) ||
        !set.ReadBlob(base.LocationOf(std::size_t(dtpIndex)), dtp, base.TotalSize(std::size_t(dtpIndex)))) {
        say("the template picture could not be read");
        return false;
    }
    // The pixels: the template's texture with the supplied data in place of its own (same size and format: a
    // 128x256 BC3, as the Outfit Manager writes), else the .dds converted as it is.
    std::vector<std::uint8_t> converted;
    std::string error;
    if (!ConvertDds(dds, true, converted, error)) {
        say(spec.picture.filename().string() + ": " + error);
        return false;
    }
    if (maxWidth) {
        std::string note;
        ShrinkPicture(converted, maxWidth, note);
        if (!note.empty()) say(note);
    }
    const std::uint32_t pixels = Get<std::uint32_t>(converted, 8);
    if (texture.size() >= 28 && Get<std::uint32_t>(texture, 4) == Get<std::uint32_t>(converted, 4) &&
        Get<std::uint32_t>(texture, 8) == pixels && Get<std::uint32_t>(texture, 16) == Get<std::uint32_t>(converted, 16) &&
        texture.size() >= pixels) {
        std::copy(converted.end() - pixels, converted.end(), texture.end() - pixels);
    } else if (texture.size() >= 28 && converted.size() >= 28) {
        // Another size (e.g. 256x512, twice the game's) or format: the template's header (its flags, class... as the
        // game's UI textures have them: a header of our own, ConvertDds's, left the picture blank in the menu) with
        // the picture's format, size and mip levels, then its pixels.
        say("a " + std::to_string(Get<std::uint16_t>(converted, 16)) + "x" + std::to_string(Get<std::uint16_t>(converted, 18)) +
            " picture (format " + std::to_string(Get<std::uint32_t>(converted, 4)) + "; the game's are 128x256 BC3)");
        {
            // (what the game's picture resource holds: its section entry, its size, the bytes after the header and
            // its last ones; for working out what else depends on the size)
            const auto& tid = base.ids[std::size_t(textureIndex)];
            auto hexOf = [&](std::size_t from, std::size_t count) {
                std::string out;
                char b[4];
                for (std::size_t k = from; k < from + count && k < texture.size(); ++k) std::snprintf(b, sizeof(b), "%02x ", texture[k]), out += b;
                return out;
            };
            say("the template texture: section body " + std::to_string(tid.bodySize) + ", subtype/ref field 0x" +
                [&] { char b[16]; std::snprintf(b, sizeof(b), "%x", tid.subTypeAndRefSize); return std::string(b); }() +
                ", flags 0x" + [&] { char b[8]; std::snprintf(b, sizeof(b), "%x", tid.flags); return std::string(b); }() +
                ", read " + std::to_string(texture.size()) + " bytes (data size in its header " +
                std::to_string(Get<std::uint32_t>(texture, 8)) + ")");
            say("the template texture's header: " + hexOf(0, 28));
            say("  after the header: " + hexOf(28, 64));
            if (texture.size() > 64) say("  its last bytes: " + hexOf(texture.size() - 64, 64));
        }
        std::vector<std::uint8_t> made(texture.begin(), texture.begin() + 28);
        Put<std::uint32_t>(made, 4, Get<std::uint32_t>(converted, 4));     // format
        Put<std::uint32_t>(made, 8, Get<std::uint32_t>(converted, 8));     // pixel data size
        Put<std::uint16_t>(made, 16, Get<std::uint16_t>(converted, 16));   // width
        Put<std::uint16_t>(made, 18, Get<std::uint16_t>(converted, 18));   // height
        // (mip levels: the template's 0 for its single level, as the game's UI pictures have it; a 1 there left the
        // picture blank. Only a picture with more levels says so.)
        if (converted[23] > 1) made[23] = converted[23];
        // The game's UI textures (flags 0x2000) carry their asset path after the header: a 256-byte field before the
        // pixels (the section is 28 + 256 + the data). Without it the game read the first 256 bytes of the pixels as
        // the name and came up 256 bytes short: no texture (the menu got a 0x0 image). Kept as the template has it.
        const std::size_t extra = texture.size() - std::min<std::size_t>(texture.size(), 28 + std::size_t(Get<std::uint32_t>(texture, 8)));
        made.insert(made.end(), texture.begin() + 28, texture.begin() + 28 + std::ptrdiff_t(extra));
        made.insert(made.end(), converted.begin() + 28, converted.end());
        texture = std::move(made);
    } else {
        say("not a 128x256 BC3 picture: used as it is");
        texture = converted;
    }
    const Key oldTexture{kTypeTexture, static_cast<std::uint32_t>(base.ids[std::size_t(textureIndex)].id)};
    const Key oldDtp{kTypeDtp, static_cast<std::uint32_t>(base.ids[std::size_t(dtpIndex)].id)};
    std::uint32_t textureId = 0, dtpId = 0;
    if (!ids.Next(kTypeTexture, textureId) || !ids.Next(kTypeDtp, dtpId)) {
        say("no free resource IDs left");
        return false;
    }
    // The DTP: its texture reference, and its own ID wherever it names itself.
    std::vector<Ref> refs;
    int pointed = 0;
    if (ParseRefs(dtp, refs))
        for (const auto& ref : refs)
            if (ref.type == kTypeTexture && ref.id == oldTexture.id) {
                SetRefId(dtp, ref, textureId);
                ++pointed;
            }
    const std::uint64_t body = RefDefinitionsSize(dtp);
    for (std::size_t at = static_cast<std::size_t>(body); body <= dtp.size() && at + 4 <= dtp.size(); at += 4)
        if (Get<std::uint32_t>(dtp, at) == oldDtp.id) Put<std::uint32_t>(dtp, at, dtpId);
    if (!pointed) say("the template's data does not refer to its texture (kept as it is)");

    out.drm = Drm();
    std::memcpy(out.drm.header, base.header, sizeof(base.header));
    out.drm.includes = base.includes;
    out.drm.dependencies = base.dependencies;
    for (std::size_t i = 0; i < base.ids.size(); ++i) {
        if (base.ids[i].locale != kGenericLocale) continue;
        DrmIdentification id = base.ids[i];
        const std::vector<std::uint8_t>* data = nullptr;
        if (static_cast<int>(i) == textureIndex) {
            id.id = static_cast<std::int32_t>(textureId);
            data = &texture;
        } else if (static_cast<int>(i) == dtpIndex) {
            id.id = static_cast<std::int32_t>(dtpId);
            data = &dtp;
        } else {
            continue;
        }
        const std::uint64_t refSize = id.type == kTypeTexture ? 0 : RefDefinitionsSize(*data);
        id.bodySize = static_cast<std::uint32_t>(data->size() - refSize);
        id.subTypeAndRefSize = (id.subTypeAndRefSize & 0xFFu) | static_cast<std::uint32_t>(refSize << 8);
        out.drm.ids.push_back(id);
        out.drm.locations.push_back(writer.AddResource(*data, Key{id.type, static_cast<std::uint32_t>(id.id)}));
        if (static_cast<int>(i) == dtpIndex) out.drm.header[7] = static_cast<std::int32_t>(out.drm.ids.size() - 1);   // the main resource
    }
    out.picture.key = spec.key;
    out.picture.imageRes = dtpId;
    out.picture.texture = textureId;
    out.picture.width = Get<std::uint16_t>(texture, 16);
    out.picture.height = Get<std::uint16_t>(texture, 18);
    out.picture.path = std::string(kPictureFolder) + "rottrframework_" + Lower(spec.objectName);
    say("texture " + std::to_string(textureId) + ", data " + std::to_string(dtpId) + " (" + out.picture.path + ")");
    return true;
}
}  // namespace


std::uint32_t PathHash(const std::string& path) { return GameArchive::PathHash(path); }

std::string ExcludeKey(const fs::path& folder, const fs::path& file) {
    const std::u8string text = file.lexically_relative(folder).generic_u8string();
    return Lower(std::string(text.begin(), text.end()));
}

const char* ArchiveFileName() { return kArchiveFile; }
const char* NfoFileName() { return kNfoFile; }

BuildResult BuildFiltered(const fs::path& gameRoot, ArchiveSet& set, const std::vector<OutfitSpec>& outfits,
                          const std::vector<RawFile>& files, std::uint32_t knownZoneCount, const LogFn& log);

BuildResult Build(const fs::path& gameRoot, const std::vector<OutfitSpec>& outfits, std::uint32_t knownZoneCount,
                  const LogFn& log, const std::vector<RawFile>& files) {
    BuildResult result;
    const fs::path archivePath = gameRoot / kArchiveFile;
    const fs::path nfoPath = gameRoot / kNfoFile;
    std::error_code ec;

    if (outfits.empty() && files.empty()) {
        // Nothing to build: remove an archive left from earlier custom outfits.
        if (fs::exists(nfoPath, ec)) {
            fs::remove(nfoPath, ec);
            fs::remove(archivePath, ec);
            log("no custom outfit models: the framework archive was removed");
        }
        result.ok = true;
        return result;
    }

    ArchiveSet set;
    std::string error;
    if (!set.Open(gameRoot, error)) {
        result.message = error;
        return result;
    }
    // A slot without a model that wears an object the game has: nothing to build (but its picture).
    std::vector<OutfitSpec> filtered;
    for (const auto& spec : outfits) {
        if (spec.cloneOnly && spec.folder.empty() && set.HasFile(DrmPath(spec.objectName))) {
            log(spec.key + ": wears the game's own " + Lower(spec.objectName));
            if (spec.picture.empty()) continue;
        }
        filtered.push_back(spec);
    }
    if (filtered.empty() && files.empty()) return Build(gameRoot, {}, knownZoneCount, log);
    return BuildFiltered(gameRoot, set, filtered, files, knownZoneCount, log);
}

BuildResult BuildFiltered(const fs::path& gameRoot, ArchiveSet& set, const std::vector<OutfitSpec>& outfits,
                          const std::vector<RawFile>& files, std::uint32_t knownZoneCount, const LogFn& log) {
    BuildResult result;
    const fs::path archivePath = gameRoot / kArchiveFile;
    const fs::path nfoPath = gameRoot / kNfoFile;
    std::error_code ec;
    std::string error;
    const int archiveId = set.maxId + 1;
    const int version = set.maxVersion + 1;
    ZoneTable zones;
    if (!ReadZoneTable(set, zones)) {
        result.message = "the game's object list (pcx64-w\\zonelist.ids) was not found in its archives";
        return result;
    }
    // New objects get the zone IDs after the game's: outfit i -> highest + 1 + i (registered so at runtime).
    // The game's table at runtime can still be longer than every zonelist.ids (seen: 9430 vs 9423), so the new IDs
    // start a little higher, and above the count the framework saw in the game last time (knownZoneCount); the unused
    // IDs in between are registered as "" like the game's own gaps.
    const std::uint32_t firstZone = std::max(zones.maxId + kZoneIdMargin, knownZoneCount) + 1;
    // Specs whose object the game has (a copy wearing it, with only a new picture) build no .drm.
    std::vector<bool> buildsObject(outfits.size());
    std::vector<std::uint32_t> zoneOf(outfits.size());
    for (std::size_t i = 0, next = 0; i < outfits.size(); ++i) {
        buildsObject[i] = !(outfits[i].cloneOnly && outfits[i].folder.empty() && set.HasFile(DrmPath(outfits[i].objectName)));
        if (!buildsObject[i]) continue;
        // (a replaced retail object keeps its own zone ID: the game knows it under that)
        auto own = outfits[i].replacesObject ? zones.ids.find(Lower(outfits[i].objectName)) : zones.ids.end();
        if (own != zones.ids.end()) zoneOf[i] = own->second;
        else if (outfits[i].replacesObject) zoneOf[i] = 0;   // (v0.9.602) a game .drm that is no object: no zone
        else zoneOf[i] = firstZone + static_cast<std::uint32_t>(next++);
    }

    Fingerprint fingerprint;
    fingerprint.AddValue(kBuilderVersion);
    fingerprint.AddValue(firstZone);
    fingerprint.AddValue(archiveId);
    fingerprint.AddValue(version);
    for (const auto& archive : set.archives) {
        fingerprint.Add(archive.name);
        fingerprint.AddValue(archive.id);
        fingerprint.AddValue(archive.version);
        fingerprint.AddValue(archive.files.size());
    }
    for (const auto& spec : outfits) {
        fingerprint.Add(spec.key);
        fingerprint.Add(Lower(spec.objectName));
        fingerprint.Add(Lower(spec.baseObjectName));
        fingerprint.AddValue(spec.detectBase);
        fingerprint.AddValue(spec.cloneOnly);
        fingerprint.AddValue(spec.replacesObject);
        for (const auto& source : spec.sources) fingerprint.Add(Lower(source));
        for (const auto& file : spec.exclude) fingerprint.Add("-" + file);
        for (const auto& file : spec.shared) {
            fingerprint.Add("shared:" + file.string());
            fingerprint.AddValue(static_cast<std::uint64_t>(fs::file_size(file, ec)));
            fingerprint.AddValue(static_cast<long long>(fs::last_write_time(file, ec).time_since_epoch().count()));
        }
        if (!spec.picture.empty()) {
            fingerprint.Add("picture:" + spec.picture.string());
            fingerprint.AddValue(static_cast<std::uint64_t>(fs::file_size(spec.picture, ec)));
            fingerprint.AddValue(static_cast<long long>(fs::last_write_time(spec.picture, ec).time_since_epoch().count()));
        }
        std::vector<std::pair<std::string, std::pair<std::uintmax_t, long long>>> files;
        for (auto it = fs::recursive_directory_iterator(spec.folder, ec);
             !spec.folder.empty() && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            const auto time = it->last_write_time(ec).time_since_epoch().count();
            files.push_back({fs::relative(it->path(), spec.folder, ec).string(),
                             {it->file_size(ec), static_cast<long long>(time)}});
        }
        std::sort(files.begin(), files.end());
        for (const auto& [name, info] : files) {
            fingerprint.Add(name);
            fingerprint.AddValue(info.first);
            fingerprint.AddValue(info.second);
        }
    }
    for (const auto& file : files) {
        fingerprint.AddValue(file.hash);
        fingerprint.Add(file.source.string());
        fingerprint.AddValue(static_cast<std::uint64_t>(fs::file_size(file.source, ec)));
        fingerprint.AddValue(static_cast<long long>(fs::last_write_time(file.source, ec).time_since_epoch().count()));
    }
    const std::string stamp = fingerprint.Hex();
    if (fs::exists(archivePath, ec) && ReadNfoFingerprint(nfoPath) == stamp) {
        result.objects = ReadNfoObjects(nfoPath);
        result.renamed = ReadNfoRenamed(nfoPath);
        result.copied = ReadNfoRenamed(nfoPath, "copied");
        result.pictures = ReadNfoPictures(nfoPath);
        result.ok = true;
        result.message = "up to date";
        return result;
    }

    UsedIds used;
    if (!ScanUsedIds(set, gameRoot / "ROTTRFramework" / "outfits" / "_ids.cache", used, log)) {
        result.message = "the game's .drm files could not be read";
        return result;
    }
    IdAllocator ids(used);
    const fs::path tempArchive = gameRoot / (std::string(kArchiveFile) + ".tmp");
    ArchiveWriter writer(archiveId);
    {
        // its file table's room: every object's .drm, every picture's, every raw file
        std::size_t most = files.size();
        for (std::size_t i = 0; i < outfits.size(); ++i) most += (buildsObject[i] ? 1 : 0) + (outfits[i].picture.empty() ? 0 : 1);
        if (!writer.Open(tempArchive, most, error)) {
            result.message = error;
            return result;
        }
    }
    BuildCache cache;
    std::vector<BuiltOutfit> built;
    std::vector<BuiltPictureDrm> pictures;
    const std::size_t pictured = static_cast<std::size_t>(
        std::count_if(outfits.begin(), outfits.end(), [](const OutfitSpec& o) { return !o.picture.empty(); }));
    const std::uint32_t pictureWidth = pictured > kManyPictures ? kManyPicturesWidth : 0;
    for (std::size_t i = 0; i < outfits.size(); ++i) {
        BuiltOutfit outfit;
        if (buildsObject[i] && BuildOutfit(set, cache, writer, outfits[i], ids, zones, zoneOf[i], outfit, log))
            built.push_back(std::move(outfit));
        BuiltPictureDrm picture;
        if (!outfits[i].picture.empty() && BuildPicture(set, writer, outfits[i], ids, picture, log, pictureWidth))
            pictures.push_back(std::move(picture));
    }
    log("read for the outfits: " + std::to_string(cache.drmReads) + " .drm file(s), " + std::to_string(cache.blobReads) +
        " resource(s), " + std::to_string(cache.archiveSearches) + " search(es) through every .drm of the game");
    // Files replaced as they are (mods' sounds: streams by their name hash).
    std::size_t rawCount = 0;
    {
        std::set<std::uint32_t> taken;
        for (const auto& file : files) {
            if (!taken.insert(file.hash).second) {
                log(file.label + ": another mod already replaces that file");
                continue;
            }
            std::vector<std::uint8_t> bytes;
            if (!ReadDisk(file.source, bytes) || bytes.empty()) {
                log(file.label + ": " + file.source.string() + " could not be read");
                continue;
            }
            writer.AddFileHash(file.hash, std::move(bytes));
            ++rawCount;
        }
    }
    if (built.empty() && pictures.empty() && !rawCount) {
        result.message = "nothing could be built";
        return result;
    }

    // Resource offsets were relative to the content start; the header and file table come first.
    const std::size_t start = writer.Start();
    auto relocate = [&](Drm& drm) {
        for (auto& location : drm.locations)
            if (location.archiveId == static_cast<std::int16_t>(archiveId)) location.offset += static_cast<std::uint32_t>(start);
    };
    for (auto& outfit : built) {
        relocate(outfit.drm);
        writer.AddFile(DrmPath(outfit.object), outfit.drm.Write());
        if (outfit.zoneId) result.objects.push_back({outfit.object, outfit.zoneId});   // (v0.9.602: not a .drm that is no object)
        result.renamed.insert(result.renamed.end(), outfit.renamed.begin(), outfit.renamed.end());
        result.copied.insert(result.copied.end(), outfit.copied.begin(), outfit.copied.end());
    }
    for (auto& picture : pictures) {
        relocate(picture.drm);
        writer.AddFile(ImageDrmPath(picture.picture.path), picture.drm.Write());
        result.pictures.push_back(picture.picture);
    }

    if (!writer.Save(error)) {
        result.message = error;
        return result;
    }
    std::ostringstream nfo;
    nfo << kOurArchiveName << " bap " << set.gameId << " package " << archiveId << " chunk 0 version " << version
        << " required 1 bigfile 1 dlcindex " << archiveId << " mount 0 unmount 1\n";
    if (!set.steamId.empty()) nfo << "custom " << set.steamId << "\n";
    nfo << "custom mod:ROTTRFramework custom outfits\n";
    for (const auto& object : result.objects)
        nfo << "custom rottrframework-object:" << object.zoneId << "," << object.name << "\n";
    for (const auto& picture : result.pictures)
        nfo << "custom rottrframework-picture:" << picture.imageRes << "," << picture.texture << "," << picture.width << ","
            << picture.height << "|" << picture.key << "|" << picture.path << "\n";
    for (const auto& renamed : result.renamed)
        nfo << "custom rottrframework-renamed:" << unsigned(renamed.type) << "," << renamed.oldId << "," << renamed.newId
            << "\n";
    for (const auto& copied : result.copied)
        nfo << "custom rottrframework-copied:" << unsigned(copied.type) << "," << copied.oldId << "," << copied.newId << "\n";
    nfo << "custom rottrframework:" << stamp << "\n";
    fs::rename(tempArchive, archivePath, ec);
    if (ec) {
        fs::remove(tempArchive, ec);
        // The game uses the archive already there: what the framework registers (object names and zone IDs, pictures,
        // IDs) comes from that one's .nfo, not from this build (its zone IDs can differ: an outfit added before
        // others moves theirs; a wrong one leaves the outfit loading forever).
        result.objects = ReadNfoObjects(nfoPath);
        result.renamed = ReadNfoRenamed(nfoPath);
        result.copied = ReadNfoRenamed(nfoPath, "copied");
        result.pictures = ReadNfoPictures(nfoPath);
        result.message = "the archive is in use (the game already opened it); restart the game to use the new outfits "
                         "(this start uses the archive built before, with " + std::to_string(result.objects.size()) +
                         " object(s))";
        return result;
    }
    {
        std::ofstream out(nfoPath, std::ios::trunc);
        out << nfo.str();
        if (!out) {
            result.message = "cannot write " + nfoPath.string();
            return result;
        }
    }
    result.ok = true;
    result.rebuilt = true;
    char text[160];
    std::snprintf(text, sizeof(text), "archive built (id %d, version %d, %zu object(s), %zu file(s), %ju bytes)", archiveId,
                  version, built.size(), rawCount, static_cast<std::uintmax_t>(fs::file_size(archivePath, ec)));
    result.message = text;
    return result;
}
}  // namespace OutfitArchive
