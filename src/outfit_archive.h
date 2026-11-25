#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// Builds ROTTRFramework's own game archive for custom outfits with new models (v0.9.311, milestone 2).
//
// For each custom outfit that ships resource files (TrRebootModTools/Blender exports such as 267210.tr10modeldata,
// name.178450.tr10material, 1485402.dds, kept under their original IDs), the builder:
//  1. reads the base outfit's .drm (pcx64-w\<base>.drm) from the game's archives (the highest .nfo version wins);
//  2. gives every supplied resource a fresh ID, and also every base resource that refers (directly or through other
//     such resources) to one of them, up to the object's root: those are copied out of the game archives with
//     their references pointed at the new IDs. The base outfit itself stays untouched;
//  3. adds resources the supplied ones refer to that are not in the base .drm, from the other outfit .drm files;
//  4. writes pcx64-w\<object>.drm (the base .drm with the swapped entries) and all new resources into
//     bigfile._mod.rottrframework.000.000.tiger + .nfo in the game folder, which the game mounts like a mod.
// The archive is only rebuilt when an input changed (a fingerprint is kept in the .nfo).
//
// Plain C++ (no game code): it runs on its own thread before the game scans its archives.
namespace OutfitArchive {
struct OutfitSpec {
    std::string key;                    // config name (folder)
    std::string objectName;             // the new object (its .drm name, lower case)
    std::string baseObjectName;         // the outfit object whose .drm is copied
    bool detectBase = false;            // baseObjectName is only a fallback: use the outfit whose .drm has most of the
                                        // supplied resource IDs (Model= not set)
    std::filesystem::path folder;       // resource files (searched recursively)
    std::vector<std::string> sources;   // more .drm names to take referenced resources from
    bool cloneOnly = false;             // no supplied files: a copy of the base outfit under the new object name
                                        // (skipped when objectName is an object the game already has)
    std::vector<std::string> exclude;   // files of the folder to leave out: relative paths, lower case, '/'
                                        // (mod.json "exclude": files the Outfit Manager found unused)
    std::filesystem::path picture;      // the campsite menu picture: a 128x256 BC3 (sRGB) .dds; empty: the base's
    std::vector<std::filesystem::path> shared;   // more resource files (the mod's shared ones it uses): as if in
                                                 // the folder (a folder file for the same resource wins)
    bool replacesObject = false;        // a mod's replacement of a retail outfit with its own model, built into the
                                        // retail object itself (objectName: the game's object, its .drm and zone ID
                                        // kept): the game's .drm is overridden by the archive's
};

// A file's path in its folder as "exclude" names it: relative, lower case, '/' separators.
std::string ExcludeKey(const std::filesystem::path& folder, const std::filesystem::path& file);

struct BuiltObject {
    std::string name;                     // the object (its .drm name)
    std::uint32_t zoneId = 0;             // its zone (object) ID: the game's highest + 1, + 2, ...
};

// A supplied resource whose ID the game uses for another resource of its type: it was given a new ID.
struct RenamedResource {
    std::uint8_t type = 0;
    std::uint32_t oldId = 0;
    std::uint32_t newId = 0;
};

// A campsite menu picture: its own image .drm in the archive (dtp::ImagePath of the outfit entry).
struct BuiltPicture {
    std::string key;                      // the outfit (OutfitSpec::key)
    std::uint32_t imageRes = 0;           // m_imageRes: the image's DTP
    std::string path;                     // m_filepath, e.g. scaleform\sharedtextures\...\rottrframework_<object>
    std::uint32_t texture = 0;            // its texture's ID (the menu loads it as "img://<ID>")
    std::uint16_t width = 0, height = 0;  // its texture's size (the game's: 128x256)
};

struct BuildResult {
    bool ok = false;
    bool rebuilt = false;                 // false: the archive on disk was already up to date
    std::vector<BuiltObject> objects;     // objects whose .drm is in the archive
    std::vector<RenamedResource> renamed; // supplied resources that got new IDs (all outfits)
    std::vector<RenamedResource> copied;  // the game's resources copied into the outfits with new IDs (the object data,
                                          // what refers to a replaced resource...): for live edits of the game's ID
    std::vector<BuiltPicture> pictures;   // custom pictures
    std::string message;
};

using LogFn = std::function<void(const std::string&)>;

// A file of the game replaced as it is (a mod's sound stream, .mul): by its name hash, from a file on disk.
struct RawFile {
    std::uint32_t hash = 0;
    std::filesystem::path source;
    std::string label;                    // for the log (mod/what)
};

// knownZoneCount: the length of the game's object (zone) table the framework saw at runtime last time (0: unknown).
BuildResult Build(const std::filesystem::path& gameRoot, const std::vector<OutfitSpec>& outfits,
                  std::uint32_t knownZoneCount, const LogFn& log, const std::vector<RawFile>& files = {});

// The tiger file name hash of a path such as "pcx64-w\\laracroft.drm".
std::uint32_t PathHash(const std::string& path);
// File names of the framework's archive (relative to the game folder).
const char* ArchiveFileName();
const char* NfoFileName();
}
