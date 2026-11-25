#pragma once
// The game's archives (tiger), .drm object files and resource data: shared by the framework's outfit archive
// builder and the standalone tools. Plain C++ (no game code).

extern "C" {
#include "puff.h"
}

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GameArchive {
namespace fs = std::filesystem;

constexpr std::uint32_t kTigerMagic = 0x53464154;   // "TAFS"
constexpr std::int32_t kTigerVersion = 4;
constexpr std::uint32_t kCdrmMagic = 0x4D524443;    // "CDRM"
constexpr std::uint32_t kNextMagic = 0x5458454E;    // "NEXT"
constexpr std::uint32_t kMaxChunk = 0x40000;
constexpr std::int32_t kDrmVersion = 22;
constexpr std::uint32_t kGenericLocale = 0xFFFFFFFFu;
constexpr const char* kArchiveFile = "bigfile._mod.rottrframework.000.000.tiger";
constexpr const char* kNfoFile = "bigfile._mod.rottrframework.000.000.nfo";
constexpr const char* kOurArchiveName = "_mod.rottrframework";

// Resource types (ResourceType in TrRebootModTools).
enum : std::uint8_t {
    kTypeAnimation = 2, kTypeBlendShapeDriver = 4, kTypeTexture = 5, kTypeSoundBank = 6, kTypeDtp = 7,
    kTypeScript = 8, kTypeShaderLib = 9, kTypeMaterial = 10, kTypeContentRef = 11, kTypeModel = 12,
    kTypeCollisionModel = 13, kTypeObjectRef = 14, kTypeAnimationLib = 15,
};

inline std::uint32_t PathHash(const std::string& path) {
    std::uint32_t hash = 0xFFFFFFFFu;
    for (unsigned char c : path) {
        hash ^= std::uint32_t(c) << 24;
        for (int bit = 0; bit < 8; ++bit) hash = (hash & 0x80000000u) ? (hash << 1) ^ 0x04C11DB7u : hash << 1;
    }
    return ~hash;
}

struct Key {
    std::uint8_t type = 0;
    std::uint32_t id = 0;
    bool operator<(const Key& o) const { return type != o.type ? type < o.type : id < o.id; }
    bool operator==(const Key& o) const { return type == o.type && id == o.id; }
};

inline std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

template <typename T> T Get(const std::vector<std::uint8_t>& b, std::size_t at) {
    T v{};
    if (at + sizeof(T) <= b.size()) std::memcpy(&v, b.data() + at, sizeof(T));
    return v;
}
template <typename T> void Put(std::vector<std::uint8_t>& b, std::size_t at, T v) {
    if (at + sizeof(T) <= b.size()) std::memcpy(b.data() + at, &v, sizeof(T));
}
template <typename T> void Append(std::vector<std::uint8_t>& b, T v) {
    const std::size_t at = b.size();
    b.resize(at + sizeof(T));
    std::memcpy(b.data() + at, &v, sizeof(T));
}
inline void Align(std::vector<std::uint8_t>& b, std::size_t alignment) {
    while (b.size() % alignment) b.push_back(0);
}

// ---- archives ----------------------------------------------------------------------------------------------------

struct FileEntry {
    std::uint32_t nameHash, locale, size, compressedSize;
    std::int16_t part, archiveId;
    std::uint32_t offset;
};
static_assert(sizeof(FileEntry) == 0x18);

struct Archive {
    fs::path basePath;
    std::string name;
    int id = 0;
    int version = 0;
    int required = 1;
    int package = 0;
    int chunk = 0;
    int rank = 0;            // mount priority: higher wins (see ArchiveSet::Open)
    int numParts = 1;
    bool language = false;   // a language archive (e.g. bigfile_ENGLISH.000.tiger): read with ReadLocalizedFile
    std::vector<FileEntry> files;
};

// Diagnostics only (a data probe): told about every archive read, the part file, offset and size (null: nothing).
inline void (*g_readObserver)(const fs::path& file, std::uint64_t offset, std::uint32_t size) = nullptr;

struct Location {
    int archiveId = 0;
    int part = 0;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    int archiveIndex = -1;   // index in ArchiveSet::archives when known (a language archive shares its game
                             // archive's ID: bigfile_ENGLISH and bigfile are both 1)
};

class ArchiveSet {
public:
    std::vector<Archive> archives;
    int gameId = 0;
    std::string steamId;
    int maxId = 0;
    int maxVersion = 0;
    int steamIdPackage = 0x7FFFFFFF;
    // The game's file system specialisation mask, (1 << text language) | platform bits (English: 0x40000001). Set
    // before Open to read like the game: a file copy is usable when (copy.locale & mask) == mask; most base-game
    // files are not "generic" (0xFFFFFFFF) but carry such a mask. 0 (the framework's builder): generic copies only.
    std::uint32_t specialisationMask = 0;

    // Every copy of a file in the archives (specialisationMask set).
    struct Copy {
        const Archive* archive = nullptr;
        const FileEntry* entry = nullptr;
        int archiveIndex = -1;
        bool usable = false;     // matches specialisationMask
        Location location() const {
            return {entry->archiveId, entry->part, entry->offset, entry->compressedSize ? entry->compressedSize : entry->size,
                    archiveIndex};
        }
    };

    // language: also mount the language archives of that language (e.g. "ENGLISH": bigfile_ENGLISH.000.tiger next to
    // bigfile.000.nfo), for ReadLocalizedFile. skipOwnArchive: leave out the framework's own archive.
    bool Open(const fs::path& root, std::string& error, const char* language = nullptr, bool skipOwnArchive = true) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const std::string file = entry.path().filename().string();
            if (file.size() < 4 || Lower(file.substr(file.size() - 4)) != ".nfo") continue;
            std::ifstream nfo(entry.path());
            std::string line;
            if (!std::getline(nfo, line)) continue;
            std::istringstream tokens(line);
            Archive archive;
            tokens >> archive.name;
            std::string key;
            int value = 0, package = 0, version = 0, bap = 0, bigfile = 1, required = 1, chunk = 0;
            while (tokens >> key >> value) {
                if (key == "package") package = value;
                else if (key == "version") version = value;
                else if (key == "required") required = value;
                else if (key == "chunk") chunk = value;
                else if (key == "bap") bap = value;
                else if (key == "bigfile") bigfile = value;
            }
            while (std::getline(nfo, line))
                if (line.rfind("custom steamID:", 0) == 0 && (steamId.empty() || package < steamIdPackage)) {
                    steamId = line.substr(7);   // the game's own (lowest package), not a DLC's
                    steamIdPackage = package;
                }
            if ((skipOwnArchive && archive.name == kOurArchiveName) || !bigfile) continue;
            if (!gameId && bap) gameId = bap;
            archive.basePath = entry.path();
            archive.basePath.replace_extension(".tiger");
            archive.version = version;
            archive.required = required;
            archive.package = package;
            archive.chunk = chunk;
            if (!ReadIndex(archive)) continue;
            maxId = std::max({maxId, archive.id, package});
            maxVersion = std::max(maxVersion, version);
            if (language) {
                // "bigfile.000.nfo" -> "bigfile_ENGLISH.000.tiger", "bigfile.update1.000.000.nfo" ->
                // "bigfile.update1.000_ENGLISH.000.tiger".
                std::string prefix = entry.path().stem().string();   // without ".nfo"
                if (prefix.size() > 4 && prefix.compare(prefix.size() - 4, 4, ".000") == 0) prefix.resize(prefix.size() - 4);
                Archive localized;
                localized.basePath = root / (prefix + "_" + language + ".000.tiger");
                localized.name = archive.name + "_" + language;
                localized.version = version;
                localized.required = required;
                localized.package = package;
                localized.chunk = chunk;
                localized.language = true;
                if (fs::is_regular_file(localized.basePath, ec) && ReadIndex(localized) && !localized.files.empty())
                    archives.push_back(std::move(localized));
            }
            archives.push_back(std::move(archive));
        }
        if (archives.empty()) {
            error = "no game archives found";
            return false;
        }
        // Mount priority, lowest first: .nfo version, then optional (DLC) over required, then package, then chunk.
        // Several archives share a version (update2 and update3 are both 3), so the version alone is not enough:
        // taking update2's laracroft.drm over update3's gave the outfit shader libraries the game no longer matches.
        std::stable_sort(archives.begin(), archives.end(), [](const Archive& a, const Archive& b) {
            if (a.version != b.version) return a.version < b.version;
            if (a.required != b.required) return a.required > b.required;
            if (a.package != b.package) return a.package < b.package;
            return a.chunk < b.chunk;
        });
        for (std::size_t i = 0; i < archives.size(); ++i) {
            archives[i].rank = static_cast<int>(i) + 1;
            if (!archives[i].language) byId_.emplace(archives[i].id, i);   // resources in .drm files name game archives
        }
        // The highest priority wins for each file.
        for (std::size_t a = 0; a < archives.size(); ++a)
            for (std::size_t f = 0; f < archives[a].files.size(); ++f) {
                const auto& file = archives[a].files[f];
                if (archives[a].language) {
                    localizedIndex_[file.nameHash] = {a, f};
                    continue;
                }
                if (file.locale != kGenericLocale) continue;
                fileIndex_[file.nameHash] = {a, f};
            }
        if (specialisationMask)
            for (std::size_t a = 0; a < archives.size(); ++a)
                for (std::size_t f = 0; f < archives[a].files.size(); ++f)
                    copies_[archives[a].files[f].nameHash].push_back({a, f});
        return true;
    }

    // All copies of a file, highest priority first (specialisationMask set; empty otherwise).
    std::vector<Copy> Copies(const std::string& path) const {
        std::vector<Copy> out;
        auto it = copies_.find(PathHash(path));
        if (it == copies_.end()) return out;
        for (const auto& [a, f] : it->second) {
            Copy copy;
            copy.archive = &archives[a];
            copy.entry = &archives[a].files[f];
            copy.archiveIndex = static_cast<int>(a);
            copy.usable = (copy.entry->locale & specialisationMask) == specialisationMask;
            out.push_back(copy);
        }
        std::stable_sort(out.begin(), out.end(), [](const Copy& x, const Copy& y) { return x.archive->rank > y.archive->rank; });
        return out;
    }
    bool ReadCopy(const Copy& copy, std::vector<std::uint8_t>& out) { return ReadBlob(copy.location(), out, copy.entry->size); }
    // The stored bytes (still compressed), for diagnostics.
    bool ReadStored(const Location& location, std::vector<std::uint8_t>& raw) { return ReadRaw(location, raw); }

    // A file of the mounted language (Open's language), e.g. pcx64-w\local\locals.bin; else the generic copy.
    bool ReadLocalizedFile(const std::string& path, std::vector<std::uint8_t>& out) {
        if (specialisationMask) return ReadFile(path, out);
        auto it = localizedIndex_.find(PathHash(path));
        if (it == localizedIndex_.end()) return ReadFile(path, out);
        const auto& file = archives[it->second.first].files[it->second.second];
        const Location location{file.archiveId, file.part, file.offset,
                                file.compressedSize ? file.compressedSize : file.size, static_cast<int>(it->second.first)};
        return ReadBlob(location, out, file.size);
    }
    std::size_t LanguageArchiveCount() const {
        std::size_t n = 0;
        for (const auto& archive : archives) n += archive.language ? 1 : 0;
        return n;
    }

    bool HasArchive(int id) const { return byId_.count(id) != 0; }
    // Mount priority of the archive with this ID (0: not a game archive).
    int RankOf(int id) const {
        auto it = byId_.find(id);
        return it == byId_.end() ? 0 : archives[it->second].rank;
    }
    // Whether this file entry is the copy the game uses (the highest-priority archive's).
    bool IsCurrent(const Archive& archive, const FileEntry& file) const {
        auto it = fileIndex_.find(file.nameHash);
        return it != fileIndex_.end() && &archives[it->second.first] == &archive &&
               &archives[it->second.first].files[it->second.second] == &file;
    }
    bool HasFile(const std::string& path) const {
        if (specialisationMask)
            for (const auto& copy : Copies(path))
                if (copy.usable) return true;
        return fileIndex_.count(PathHash(path)) != 0;
    }

    // A file such as pcx64-w\laracroft.drm, decompressed.
    bool ReadFile(const std::string& path, std::vector<std::uint8_t>& out, Location* where = nullptr) {
        if (specialisationMask) {   // the highest-priority usable copy, like the game
            for (const auto& copy : Copies(path))
                if (copy.usable) {
                    if (where) *where = copy.location();
                    return ReadCopy(copy, out);
                }
            return false;
        }
        auto it = fileIndex_.find(PathHash(path));
        if (it == fileIndex_.end()) return false;
        const auto& file = archives[it->second.first].files[it->second.second];
        Location location{file.archiveId, file.part, file.offset,
                          file.compressedSize ? file.compressedSize : file.size};
        if (where) *where = location;
        return ReadBlob(location, out, file.size);
    }

    // A resource blob (CDRM or raw), decompressed. expected: uncompressed size (0 = any).
    bool ReadBlob(const Location& location, std::vector<std::uint8_t>& out, std::uint32_t expected) {
        std::vector<std::uint8_t> raw;
        if (!ReadRaw(location, raw)) return false;
        if (raw.size() >= 16 && Get<std::uint32_t>(raw, 0) == kCdrmMagic) return Decompress(raw, out);
        out = std::move(raw);
        return !expected || out.size() == expected;
    }

    // Only the first chunk of a resource (enough for a texture header).
    bool ReadBlobHead(const Location& location, std::vector<std::uint8_t>& out) {
        Location head = location;
        head.size = std::min<std::uint32_t>(location.size, kMaxChunk + 0x1000);
        std::vector<std::uint8_t> raw;
        if (!ReadRaw(head, raw)) return false;
        if (raw.size() >= 16 && Get<std::uint32_t>(raw, 0) == kCdrmMagic) return Decompress(raw, out, true);
        out = std::move(raw);
        return true;
    }

    // The first `need` bytes of a resource or file (fewer when it is shorter), decompressing only what they take.
    bool ReadPrefix(const Location& location, std::size_t need, std::vector<std::uint8_t>& out) {
        out.clear();
        std::vector<std::uint8_t> raw;
        Location part = location;
        part.size = std::min<std::uint32_t>(location.size, 16);
        if (!ReadRaw(part, raw) || raw.size() < 16) return false;
        if (Get<std::uint32_t>(raw, 0) != kCdrmMagic) {
            part.size = static_cast<std::uint32_t>(std::min<std::size_t>(location.size, need));
            return ReadRaw(part, out);
        }
        const std::uint32_t chunks = Get<std::uint32_t>(raw, 8);
        if (!chunks || chunks > 0x10000) return false;
        const std::size_t tableEnd = 16 + 8 * std::size_t(chunks);
        part.size = static_cast<std::uint32_t>(std::min<std::size_t>(location.size, tableEnd));
        if (!ReadRaw(part, raw) || raw.size() < tableEnd) return false;
        // The chunks that hold the prefix.
        std::size_t at = (tableEnd + 15) & ~std::size_t(15), produced = 0, rawEnd = at;
        std::uint32_t used = 0;
        for (; used < chunks && produced < need; ++used) {
            produced += Get<std::uint32_t>(raw, 16 + 8 * used) >> 8;
            rawEnd = at + Get<std::uint32_t>(raw, 16 + 8 * used + 4);
            at = (rawEnd + 15) & ~std::size_t(15);
        }
        if (rawEnd > location.size) return false;
        part.size = static_cast<std::uint32_t>(rawEnd);
        if (!ReadRaw(part, raw)) return false;
        at = (tableEnd + 15) & ~std::size_t(15);
        for (std::uint32_t c = 0; c < used; ++c) {
            const std::uint32_t packed = Get<std::uint32_t>(raw, 16 + 8 * c);
            const std::uint32_t usize = packed >> 8;
            const std::uint32_t csize = Get<std::uint32_t>(raw, 16 + 8 * c + 4);
            const std::size_t take = std::min<std::size_t>(usize, need - out.size());
            const std::size_t start = out.size();
            out.resize(start + take);
            if ((packed & 0xFF) == 1 || csize == usize) {
                std::memcpy(out.data() + start, raw.data() + at, take);
            } else {
                if (csize < 2) return false;
                unsigned long destLength = static_cast<unsigned long>(take);
                unsigned long sourceLength = csize - 2;
                // 1 = the output is full (only the prefix was wanted): puff fills it before stopping.
                const int status = puff(out.data() + start, &destLength, raw.data() + at + 2, &sourceLength);
                if (status != 0 && !(status == 1 && take < usize)) return false;
            }
            at = (at + csize + 15) & ~std::size_t(15);
        }
        return true;
    }

private:
    std::map<int, std::size_t> byId_;
    std::unordered_map<std::uint32_t, std::pair<std::size_t, std::size_t>> fileIndex_;
    std::unordered_map<std::uint32_t, std::pair<std::size_t, std::size_t>> localizedIndex_;
    std::unordered_map<std::uint32_t, std::vector<std::pair<std::size_t, std::size_t>>> copies_;
    std::map<std::pair<int, int>, std::unique_ptr<std::ifstream>> streams_;

    static bool ReadIndex(Archive& archive) {
        std::ifstream in(archive.basePath, std::ios::binary);
        if (!in) return false;
        std::int32_t header[5]{};
        in.read(reinterpret_cast<char*>(header), sizeof(header));
        if (!in || static_cast<std::uint32_t>(header[0]) != kTigerMagic || header[1] != kTigerVersion) return false;
        archive.numParts = header[2];
        archive.id = header[4];
        in.seekg(0x20, std::ios::cur);   // platform
        archive.files.resize(static_cast<std::size_t>(std::max(header[3], 0)));
        in.read(reinterpret_cast<char*>(archive.files.data()),
                static_cast<std::streamsize>(archive.files.size() * sizeof(FileEntry)));
        return static_cast<bool>(in);
    }

    static fs::path PartPath(const fs::path& basePath, int part) {
        std::string path = basePath.string();
        const std::string tail = ".000.tiger";
        if (part == 0 || path.size() < tail.size()) return basePath;
        char replacement[16];
        std::snprintf(replacement, sizeof(replacement), ".%03d.tiger", part);
        path.replace(path.size() - tail.size(), tail.size(), replacement);
        return fs::path(path);
    }

    bool ReadRaw(const Location& location, std::vector<std::uint8_t>& raw) {
        std::size_t index = 0;
        if (location.archiveIndex >= 0 && static_cast<std::size_t>(location.archiveIndex) < archives.size()) {
            index = static_cast<std::size_t>(location.archiveIndex);
        } else {
            auto archive = byId_.find(location.archiveId);
            if (archive == byId_.end()) return false;
            index = archive->second;
        }
        // (several threads read at once: the streams are shared; decompressing happens outside this lock)
        static std::mutex streamsMutex;
        std::lock_guard<std::mutex> lock(streamsMutex);
        auto& stream = streams_[{static_cast<int>(index), location.part}];
        if (!stream) {
            stream = std::make_unique<std::ifstream>(PartPath(archives[index].basePath, location.part), std::ios::binary);
            if (!*stream) return false;
        }
        if (g_readObserver) g_readObserver(PartPath(archives[index].basePath, location.part), location.offset, location.size);
        raw.resize(location.size);
        stream->clear();
        stream->seekg(location.offset);
        stream->read(reinterpret_cast<char*>(raw.data()), location.size);
        return static_cast<bool>(*stream);
    }

    static bool Decompress(const std::vector<std::uint8_t>& raw, std::vector<std::uint8_t>& out, bool firstOnly = false) {
        const std::uint32_t chunks = Get<std::uint32_t>(raw, 8);
        if (chunks > 0x10000) return false;
        std::size_t at = (16 + 8 * std::size_t(chunks) + 15) & ~std::size_t(15);
        out.clear();
        for (std::uint32_t c = 0; c < chunks; ++c) {
            const std::uint32_t packed = Get<std::uint32_t>(raw, 16 + 8 * c);
            const std::uint32_t usize = packed >> 8;
            const std::uint32_t csize = Get<std::uint32_t>(raw, 16 + 8 * c + 4);
            if (at + csize > raw.size()) return firstOnly && c > 0;
            const std::size_t start = out.size();
            out.resize(start + usize);
            if ((packed & 0xFF) == 1 || csize == usize) {
                std::memcpy(out.data() + start, raw.data() + at, usize);
            } else {
                if (csize < 2) return false;
                unsigned long destLength = usize;
                unsigned long sourceLength = csize - 2;
                if (puff(out.data() + start, &destLength, raw.data() + at + 2, &sourceLength) != 0 ||
                    destLength != usize)
                    return false;
            }
            at = (at + csize + 15) & ~std::size_t(15);
            if (firstOnly) return true;
        }
        return true;
    }
};

// ---- .drm -----------------------------------------------------------------------------------------------------------

#pragma pack(push, 1)
struct DrmIdentification {
    std::uint32_t bodySize;
    std::uint8_t type;
    std::uint8_t flags;
    std::int16_t padding;
    std::uint32_t subTypeAndRefSize;   // bits 1..7 subtype, bits 8.. size of the reference definitions
    std::int32_t id;
    std::uint32_t locale;
};
struct DrmLocation {
    std::int32_t uniqueKey;            // type << 24 | id
    std::int32_t padding;
    std::int16_t part;
    std::int16_t archiveId;
    std::uint32_t offset;
    std::uint32_t size;
    std::uint32_t decompressionOffset;
};
#pragma pack(pop)
static_assert(sizeof(DrmIdentification) == 0x14 && sizeof(DrmLocation) == 0x18);

struct Drm {
    std::int32_t header[8]{};   // version, include length, dependency length, padding, size, flags, count, main index
    std::vector<DrmIdentification> ids;
    std::vector<DrmLocation> locations;
    std::vector<std::uint8_t> dependencies, includes;

    bool Parse(const std::vector<std::uint8_t>& data) {
        if (data.size() < 0x20) return false;
        std::memcpy(header, data.data(), sizeof(header));
        if (header[0] != kDrmVersion || header[6] < 0) return false;
        const std::size_t n = static_cast<std::size_t>(header[6]);
        std::size_t at = 0x20;
        const std::size_t need = at + n * sizeof(DrmIdentification) + std::size_t(std::max(header[1], 0)) +
                                 std::size_t(std::max(header[2], 0)) + n * sizeof(DrmLocation);
        if (need > data.size()) return false;
        ids.resize(n);
        std::memcpy(ids.data(), data.data() + at, n * sizeof(DrmIdentification));
        at += n * sizeof(DrmIdentification);
        dependencies.assign(data.begin() + at, data.begin() + at + header[2]);
        at += header[2];
        includes.assign(data.begin() + at, data.begin() + at + header[1]);
        at += header[1];
        locations.resize(n);
        std::memcpy(locations.data(), data.data() + at, n * sizeof(DrmLocation));
        return true;
    }

    std::vector<std::uint8_t> Write() const {
        std::int32_t h[8];
        std::memcpy(h, header, sizeof(h));
        h[1] = static_cast<std::int32_t>(includes.size());
        h[2] = static_cast<std::int32_t>(dependencies.size());
        h[6] = static_cast<std::int32_t>(ids.size());
        std::vector<std::uint8_t> out(sizeof(h));
        std::memcpy(out.data(), h, sizeof(h));
        const auto* idBytes = reinterpret_cast<const std::uint8_t*>(ids.data());
        out.insert(out.end(), idBytes, idBytes + ids.size() * sizeof(DrmIdentification));
        out.insert(out.end(), dependencies.begin(), dependencies.end());
        out.insert(out.end(), includes.begin(), includes.end());
        const auto* locBytes = reinterpret_cast<const std::uint8_t*>(locations.data());
        out.insert(out.end(), locBytes, locBytes + locations.size() * sizeof(DrmLocation));
        return out;
    }

    Location LocationOf(std::size_t i) const {
        return {locations[i].archiveId, locations[i].part, locations[i].offset, locations[i].size};
    }
    std::uint32_t RefSize(std::size_t i) const { return ids[i].subTypeAndRefSize >> 8; }
    std::uint32_t TotalSize(std::size_t i) const { return ids[i].bodySize + RefSize(i); }
};

// ---- references inside a resource ------------------------------------------------------------------------------------

struct Ref {
    enum Kind { Wide, IntPatch, ShortPatch, Packed } kind;
    std::uint8_t type;
    std::uint32_t id;
    std::size_t defAt;    // the definition, in the file
    std::size_t bodyAt;   // the slot, in the file (wide: 0)
};

inline std::uint64_t RefDefinitionsSize(const std::vector<std::uint8_t>& file) {
    if (file.size() < 20) return ~0ull;
    const auto c = [&](int i) { return std::uint64_t(Get<std::uint32_t>(file, 4 * i)); };
    return 20 + c(0) * 8 + c(1) * 16 + c(2) * 4 + c(3) * 8 + c(4) * 4;
}

inline bool ParseRefs(const std::vector<std::uint8_t>& file, std::vector<Ref>& refs) {
    refs.clear();
    const std::uint64_t refSize = RefDefinitionsSize(file);
    if (refSize > file.size()) return false;
    const std::size_t body = static_cast<std::size_t>(refSize);
    const std::uint32_t internals = Get<std::uint32_t>(file, 0), wides = Get<std::uint32_t>(file, 4),
                        ints = Get<std::uint32_t>(file, 8), shorts = Get<std::uint32_t>(file, 12),
                        packeds = Get<std::uint32_t>(file, 16);
    std::size_t at = 20 + std::size_t(internals) * 8;
    for (std::uint32_t i = 0; i < wides; ++i, at += 16)
        refs.push_back({Ref::Wide, static_cast<std::uint8_t>(Get<std::uint32_t>(file, at + 4)),
                        Get<std::uint32_t>(file, at + 8), at, 0});
    for (std::uint32_t i = 0; i < ints; ++i, at += 4) {
        const std::uint32_t def = Get<std::uint32_t>(file, at);
        const std::size_t slot = body + std::size_t(def & 0x1FFFFFFu) * 4;
        refs.push_back({Ref::IntPatch, static_cast<std::uint8_t>(def >> 25), Get<std::uint32_t>(file, slot), at, slot});
    }
    for (std::uint32_t i = 0; i < shorts; ++i, at += 8) {
        const std::uint16_t type = Get<std::uint16_t>(file, at);
        const std::uint32_t high = Get<std::uint16_t>(file, at + 2);
        const std::size_t slot = body + Get<std::uint32_t>(file, at + 4);
        refs.push_back({Ref::ShortPatch, static_cast<std::uint8_t>(type), (high << 16) | Get<std::uint16_t>(file, slot),
                        at, slot});
    }
    for (std::uint32_t i = 0; i < packeds; ++i, at += 4) {
        const std::uint32_t def = Get<std::uint32_t>(file, at);
        const std::size_t slot = body + std::size_t(def & 0x1FFFFFFu) * 4;
        refs.push_back({Ref::Packed, static_cast<std::uint8_t>(def >> 25),
                        Get<std::uint32_t>(file, slot) & 0x7FFFFFFFu, at, slot});
    }
    return true;
}

inline void SetRefId(std::vector<std::uint8_t>& file, const Ref& ref, std::uint32_t id) {
    switch (ref.kind) {
        case Ref::Wide: Put<std::uint32_t>(file, ref.defAt + 8, id); break;
        case Ref::IntPatch: Put<std::uint32_t>(file, ref.bodyAt, id); break;
        case Ref::ShortPatch:
            Put<std::uint16_t>(file, ref.defAt + 2, static_cast<std::uint16_t>(id >> 16));
            Put<std::uint16_t>(file, ref.bodyAt, static_cast<std::uint16_t>(id & 0xFFFF));
            break;
        case Ref::Packed:
            Put<std::uint32_t>(file, ref.bodyAt, (Get<std::uint32_t>(file, ref.bodyAt) & 0x80000000u) | (id & 0x7FFFFFFFu));
            break;
    }
}


// DDS -> the game's texture resource (PCD9 header + the same pixel data). srgb: use the sRGB variant of the format.
inline bool ConvertDds(const std::vector<std::uint8_t>& dds, bool srgb, std::vector<std::uint8_t>& out, std::string& error) {
    if (dds.size() < 128 || Get<std::uint32_t>(dds, 0) != 0x20534444) {
        error = "not a DDS file";
        return false;
    }
    const std::uint32_t height = Get<std::uint32_t>(dds, 12), width = Get<std::uint32_t>(dds, 16);
    const std::uint32_t mips = Get<std::uint32_t>(dds, 28);
    const std::uint32_t pfFlags = Get<std::uint32_t>(dds, 80), fourCC = Get<std::uint32_t>(dds, 84);
    const std::uint32_t bits = Get<std::uint32_t>(dds, 88), rMask = Get<std::uint32_t>(dds, 92),
                        gMask = Get<std::uint32_t>(dds, 96), bMask = Get<std::uint32_t>(dds, 100),
                        aMask = Get<std::uint32_t>(dds, 104);
    auto four = [](const char* s) { return std::uint32_t(s[0]) | std::uint32_t(s[1]) << 8 | std::uint32_t(s[2]) << 16 | std::uint32_t(s[3]) << 24; };
    std::uint32_t format = 0;
    std::size_t dataAt = 128;
    if (pfFlags & 0x4) {
        if (fourCC == four("DX10")) {
            format = Get<std::uint32_t>(dds, 128);
            dataAt = 148;
        } else if (fourCC == four("DXT1")) format = 71;
        else if (fourCC == four("DXT3")) format = 74;
        else if (fourCC == four("DXT5")) format = 77;
        else if (fourCC == four("BC4U") || fourCC == four("ATI1")) format = 80;
        else if (fourCC == four("BC5U") || fourCC == four("ATI2")) format = 83;
    } else if (pfFlags & 0x40) {
        if (bits == 8 && rMask == 0xFF) format = 61;
        else if (bits == 16 && rMask == 0xFF && gMask == 0xFF00) format = 49;
        else if (bits == 32 && bMask == 0xFF && gMask == 0xFF00 && rMask == 0xFF0000 && aMask == 0xFF000000u) format = 87;
    }
    if (!format) {
        error = "unsupported DDS pixel format";
        return false;
    }
    if (srgb) {
        switch (format) {
            case 28: format = 29; break;
            case 71: format = 72; break;
            case 74: format = 75; break;
            case 77: format = 78; break;
            case 98: format = 99; break;
            default: break;
        }
    }
    const std::uint32_t size = static_cast<std::uint32_t>(dds.size() - dataAt);
    out.clear();
    Append<std::uint32_t>(out, 0x39444350u);   // "PCD9"
    Append<std::uint32_t>(out, format);
    Append<std::uint32_t>(out, size);
    Append<std::uint32_t>(out, 0);                     // high-res mip levels
    Append<std::uint16_t>(out, static_cast<std::uint16_t>(width));
    Append<std::uint16_t>(out, static_cast<std::uint16_t>(height));
    Append<std::uint16_t>(out, 1);                     // volume depth
    Append<std::uint8_t>(out, 0);                      // depth
    Append<std::uint8_t>(out, static_cast<std::uint8_t>(std::max<std::uint32_t>(mips, 1)));
    Append<std::uint16_t>(out, 0x27);                  // flags
    Append<std::uint8_t>(out, 1);                      // class
    Append<std::uint8_t>(out, 0);                      // tile mode
    out.insert(out.end(), dds.begin() + dataAt, dds.end());
    return true;
}

inline bool IsSrgb(std::uint32_t format) { return format == 29 || format == 72 || format == 75 || format == 78 || format == 99; }

struct ZoneTable {
    std::map<std::string, std::uint32_t> ids;   // lower-case name -> zone ID
    std::uint32_t maxId = 0;
};

// Every copy in the archives (the base game's and the DLCs' differ; the game ends up with the largest): names from
// the newest archive, the highest ID of all.
inline bool ReadZoneTable(ArchiveSet& set, ZoneTable& table) {
    const std::uint32_t hash = PathHash("pcx64-w\\zonelist.ids");
    std::vector<std::pair<int, const FileEntry*>> copies;
    for (const auto& archive : set.archives)
        for (const auto& file : archive.files)
            if (file.nameHash == hash) copies.push_back({archive.rank, &file});
    std::sort(copies.begin(), copies.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [version, file] : copies) {
        std::vector<std::uint8_t> bytes;
        const Location location{file->archiveId, file->part, file->offset,
                                file->compressedSize ? file->compressedSize : file->size};
        if (!set.ReadBlob(location, bytes, file->size)) continue;
        std::istringstream in(std::string(bytes.begin(), bytes.end()));
        std::string line;
        std::getline(in, line);   // "<version>,<count>"
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            const std::size_t comma = line.find(',');
            if (comma == std::string::npos) continue;
            const auto id = static_cast<std::uint32_t>(std::strtoul(line.c_str(), nullptr, 10));
            if (!id) continue;
            table.ids[Lower(line.substr(comma + 1))] = id;
            table.maxId = std::max(table.maxId, id);
        }
    }
    return table.maxId != 0;
}

}  // namespace GameArchive
