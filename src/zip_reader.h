#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Minimal .zip reader for outfit packages: stored and deflated entries (puff), no encryption, no zip64.
namespace ZipReader {
struct Entry {
    std::string name;           // as stored ('/' separators); UTF-8 (converted from the OEM code page if needed)
    std::uint16_t method = 0;   // 0 stored, 8 deflated
    std::uint16_t flags = 0;
    std::uint32_t crc = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t size = 0;
    std::uint32_t localHeader = 0;
    bool directory = false;
};

class Archive {
public:
    bool Open(const std::filesystem::path& file, std::string& error);
    const std::vector<Entry>& Entries() const { return entries_; }
    bool Read(const Entry& entry, std::vector<std::uint8_t>& out, std::string& error) const;

private:
    std::vector<std::uint8_t> data_;
    std::vector<Entry> entries_;
};

// Extracts every entry under `destination` (created if needed). Entries with absolute paths or ".." are refused.
bool ExtractAll(const std::filesystem::path& zip, const std::filesystem::path& destination, std::string& error);

std::uint32_t Crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0);
}  // namespace ZipReader
