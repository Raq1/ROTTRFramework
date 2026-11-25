#include "zip_reader.h"

extern "C" {
#include "puff.h"
}

#include <cstring>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace fs = std::filesystem;

namespace ZipReader {
namespace {
std::uint16_t U16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t U32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// Names without the UTF-8 flag are in the OEM code page (what Windows' "Compressed folder" writes).
std::string NameToUtf8(const char* name, std::size_t length, bool utf8) {
    std::string text(name, length);
#ifdef _WIN32
    bool ascii = true;
    for (char c : text) ascii = ascii && static_cast<unsigned char>(c) < 0x80;
    if (utf8 || ascii || text.empty()) return text;
    const int wide = MultiByteToWideChar(CP_OEMCP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (wide <= 0) return text;
    std::wstring w(static_cast<std::size_t>(wide), L'\0');
    MultiByteToWideChar(CP_OEMCP, 0, text.data(), static_cast<int>(text.size()), w.data(), wide);
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, w.data(), wide, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return text;
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), wide, out.data(), bytes, nullptr, nullptr);
    return out;
#else
    (void)utf8;
    return text;
#endif
}

fs::path FromUtf8(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }
}  // namespace

std::uint32_t Crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc) {
    static std::uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

bool Archive::Open(const fs::path& file, std::string& error) {
    data_.clear();
    entries_.clear();
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "cannot open the file";
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff length = in.tellg();
    if (length < 22 || length > 0x7FFFFFFF) {
        error = length < 22 ? "not a zip file" : "the zip file is too large (2 GB at most)";
        return false;
    }
    data_.resize(static_cast<std::size_t>(length));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(data_.data()), length);
    if (!in) {
        error = "cannot read the file";
        return false;
    }

    // End of central directory: the last "PK\5\6" (a comment of up to 64 KB may follow it).
    const std::size_t size = data_.size();
    std::size_t eocd = std::string::npos;
    const std::size_t lowest = size > 22 + 0xFFFF ? size - 22 - 0xFFFF : 0;
    for (std::size_t at = size - 22 + 1; at-- > lowest;)
        if (U32(&data_[at]) == 0x06054B50u) {
            eocd = at;
            break;
        }
    if (eocd == std::string::npos) {
        error = "not a zip file (no central directory)";
        return false;
    }
    const std::uint16_t count = U16(&data_[eocd + 10]);
    const std::uint32_t dirSize = U32(&data_[eocd + 12]);
    const std::uint32_t dirOffset = U32(&data_[eocd + 16]);
    if (count == 0xFFFF || dirOffset == 0xFFFFFFFFu) {
        error = "zip64 archives are not supported";
        return false;
    }
    if (std::uint64_t(dirOffset) + dirSize > eocd) {
        error = "the zip file is damaged (central directory out of range)";
        return false;
    }
    std::size_t at = dirOffset;
    for (unsigned i = 0; i < count; ++i) {
        if (at + 46 > eocd || U32(&data_[at]) != 0x02014B50u) {
            error = "the zip file is damaged (central directory entry)";
            return false;
        }
        const std::uint8_t* p = &data_[at];
        Entry entry;
        entry.flags = U16(p + 8);
        entry.method = U16(p + 10);
        entry.crc = U32(p + 16);
        entry.compressedSize = U32(p + 20);
        entry.size = U32(p + 24);
        const std::uint16_t nameLength = U16(p + 28);
        const std::uint16_t extraLength = U16(p + 30);
        const std::uint16_t commentLength = U16(p + 32);
        entry.localHeader = U32(p + 42);
        if (at + 46 + nameLength > eocd) {
            error = "the zip file is damaged (entry name)";
            return false;
        }
        entry.name = NameToUtf8(reinterpret_cast<const char*>(p + 46), nameLength, (entry.flags & 0x800) != 0);
        for (char& c : entry.name)
            if (c == '\\') c = '/';
        entry.directory = !entry.name.empty() && entry.name.back() == '/';
        entries_.push_back(std::move(entry));
        at += 46u + nameLength + extraLength + commentLength;
    }
    return true;
}

bool Archive::Read(const Entry& entry, std::vector<std::uint8_t>& out, std::string& error) const {
    out.clear();
    if (entry.flags & 1) {
        error = entry.name + ": encrypted entries are not supported";
        return false;
    }
    const std::size_t header = entry.localHeader;
    if (header + 30 > data_.size() || U32(&data_[header]) != 0x04034B50u) {
        error = entry.name + ": damaged local header";
        return false;
    }
    const std::size_t start = header + 30 + U16(&data_[header + 26]) + U16(&data_[header + 28]);
    if (start + entry.compressedSize > data_.size()) {
        error = entry.name + ": data out of range";
        return false;
    }
    out.resize(entry.size);
    if (entry.method == 0) {
        if (entry.compressedSize != entry.size) {
            error = entry.name + ": damaged stored entry";
            return false;
        }
        if (entry.size) std::memcpy(out.data(), &data_[start], entry.size);
    } else if (entry.method == 8) {
        unsigned long destLength = entry.size;
        unsigned long sourceLength = entry.compressedSize;
        // An empty output buffer: puff needs a valid pointer (it only writes destLength bytes).
        unsigned char dummy = 0;
        const int status = puff(entry.size ? out.data() : &dummy, &destLength, &data_[start], &sourceLength);
        if (status != 0 || destLength != entry.size) {
            error = entry.name + ": cannot decompress (error " + std::to_string(status) + ")";
            return false;
        }
    } else {
        error = entry.name + ": compression method " + std::to_string(entry.method) +
                " is not supported (use the standard \"Deflate\" method)";
        return false;
    }
    if (Crc32(out.data(), out.size()) != entry.crc) {
        error = entry.name + ": checksum mismatch (the zip file is damaged)";
        return false;
    }
    return true;
}

bool ExtractAll(const fs::path& zip, const fs::path& destination, std::string& error) {
    Archive archive;
    if (!archive.Open(zip, error)) return false;
    std::error_code ec;
    fs::create_directories(destination, ec);
    std::vector<std::uint8_t> buffer;
    for (const Entry& entry : archive.Entries()) {
        const fs::path relative = FromUtf8(entry.name).lexically_normal();
        bool safe = !relative.empty() && relative.is_relative() && !relative.has_root_name() &&
                    !relative.has_root_directory();
        for (const auto& part : relative) safe = safe && part != "..";
        if (!safe) {
            error = entry.name + ": unsafe path in the zip file";
            return false;
        }
        const fs::path target = destination / relative;
        if (entry.directory) {
            fs::create_directories(target, ec);
            continue;
        }
        if (!archive.Read(entry, buffer, error)) return false;
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (out && !buffer.empty()) out.write(reinterpret_cast<const char*>(buffer.data()),
                                              static_cast<std::streamsize>(buffer.size()));
        if (!out) {
            error = entry.name + ": cannot write the file (disk full or no access?)";
            return false;
        }
    }
    return true;
}
}  // namespace ZipReader
