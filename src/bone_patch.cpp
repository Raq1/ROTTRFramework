#include "bone_patch.h"
#include "game_build.h"
#include "log.h"
#include <Windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <algorithm>
#include <iterator>
#include <memory>
#include <new>
#include <cmath>

#pragma intrinsic(_ReturnAddress)

namespace {
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using i32 = std::int32_t;
using u64 = std::uint64_t;
using uptr = std::uintptr_t;

constexpr uptr kRvaCalcSkeletonMatrices = 0x003D1860ull;
constexpr uptr kRvaGetBoneUsageMap      = 0x0027AA10ull;
constexpr uptr kRvaSetMatrices          = 0x00280930ull;
constexpr uptr kRvaSetPrevMatrices      = 0x00280CC0ull;
constexpr uptr kRvaObjectZoneNumSegments = 0x0023EC80ull;
constexpr uptr kCallerDynamicHairScratch = 0x00287B10ull;
constexpr uptr kRvaDrawableBoneConstantsClamp = 0x003D9B4Aull;
constexpr uptr kRvaInstanceBoneConstantsClamp = 0x003D9C49ull;

constexpr u32 kMaxModelBones = 8191u;
constexpr u32 kStockMaxBones = 256u;



constexpr u32 kMinAutoDiscoveredModelBones = 8u;
constexpr u32 kUsageMapWords = 256u;
constexpr u32 kUsageCacheEntries = 256u;
constexpr u32 kModelNumBonesOffset = 0xB4u;

constexpr u8 kCalcPrologue[17] = {
    0x40,0x55,0x56,0x57,0x48,0x8D,0xAC,0x24,0xB0,0x70,0xFF,0xFF,0xB8,0x50,0x90,0x00,0x00
};
constexpr u8 kUsageMapPrologue[14] = {
    0x48,0x83,0xEC,0x28,0x48,0x8B,0x49,0x48,0x48,0x8B,0x01,0xFF,0x50,0x70
};
constexpr u8 kObjectZoneNumSegmentsPrologue[15] = {
    0x48,0x8B,0x41,0x08,0x48,0x8B,0x48,0x58,0x48,0x8B,0x41,0x78,0x48,0x85,0xC0
};
constexpr u8 kDrawableBoneConstantsClamp[10] = {
    0xB8,0xFF,0x00,0x00,0x00,0x3B,0xD8,0x0F,0x47,0xD8
};
constexpr u8 kInstanceBoneConstantsClamp[10] = {
    0xB8,0xFF,0x00,0x00,0x00,0x3B,0xD8,0x0F,0x4F,0xD8
};

struct alignas(16) Vec4 { float x, y, z, w; };
struct alignas(16) Matrix { Vec4 row[4]; };
struct SegmentList { u32 count; u32 pad; const u8* segments; };
struct UsageCacheEntry {
    std::atomic<u32> state{0}; 
    const u8* model = nullptr;
    u16 numBones = 0;
    u16 pad = 0;
    u32 stockWords[8]{};
    u32 words[kUsageMapWords]{};
};
static_assert(sizeof(Vec4) == 0x10);
static_assert(sizeof(Matrix) == 0x40);
static_assert(sizeof(SegmentList) == 0x10);

using CalcFn = void (*)(const Matrix*, const SegmentList*, const Matrix*, const Matrix*, u32, void*);
using UsageFn = u32* (*)(void*);
using SetMatricesFn = void (*)(void*, const Matrix*, u32, u32);
using NumSegmentsFn = i32 (*)(void*);

CalcFn g_originalCalc = nullptr;
UsageFn g_originalUsage = nullptr;
SetMatricesFn g_setMatrices = nullptr;
SetMatricesFn g_setPrevMatrices = nullptr;
NumSegmentsFn g_originalObjectZoneNumSegments = nullptr;
UsageCacheEntry g_usageCache[kUsageCacheEntries];
std::atomic_bool g_installed{false};
std::atomic_bool g_loggedDynamicHairClamp{false};
std::atomic_bool g_loggedUsageMap{false};
std::atomic_bool g_loggedInvalidCalc{false};






std::atomic_bool g_preserveModelBonePositions{false};



std::atomic_bool g_cutsceneActive{false};


std::atomic_bool g_preserveMatched{false};
std::atomic<uptr> g_preserveLastCallerRva{0};
std::atomic<u32> g_preserveLastBoneCount{0};
std::atomic<u32> g_preserveLastMatchKind{0}; 
std::atomic<u32> g_preserveCachedModelCount{0};

struct PreserveBoneOp {
    u32 bone = 0;
    u32 parent = 0;
    Vec4 localOffset{};
};

struct PreserveModelCache {
    uptr segmentList = 0;
    uptr segments = 0;
    uptr boneMapPairs = 0;
    u32 count = 0;
    u32 boneMapPairCount = 0;
    u32 animatedBoneCount = 0;
    u32 physicsBoneCount = 0;
    bool haveBoneMap = false;
    bool priorityPlayer = false;
    // Corrected: the player and the objects with a DRAWABLE component (characters). Props have none and are cached
    // only to answer their discovery requests: the cutscene places their pieces itself.
    bool correct = true;
    std::vector<PreserveBoneOp> ops;
    
    
    std::vector<u8> animatedBoneMask;
    
    
    std::vector<u32> globalBoneIds;

    // bones whose native translation animates (latched: they keep the cutscene's place)
    std::unique_ptr<std::atomic<u64>[]> translatedBoneWords;
    u32 translatedBoneWordCount = 0u;

    
    
    
    u64 skeletonSignature = 0u;
    u64 quickSkeletonSignature = 0u;
    u64 boneMapSignature = 0u;
};




constexpr u32 kPreserveCacheTableEntries = 2048u;
static_assert((kPreserveCacheTableEntries & (kPreserveCacheTableEntries - 1u)) == 0u,
              "preserve cache table size must be a power of two");
std::atomic<const PreserveModelCache*> g_preserveCacheTable[kPreserveCacheTableEntries]{};
std::vector<std::unique_ptr<PreserveModelCache>> g_preserveOwnedCaches;






constexpr u32 kDiscoveryRequestEntries = 256u;
constexpr uptr kDiscoveryTombstone = 1u;
static_assert((kDiscoveryRequestEntries & (kDiscoveryRequestEntries - 1u)) == 0u,
              "discovery request table size must be a power of two");
struct DiscoveryRequestSlot {
    std::atomic<uptr> segments{0};
    std::atomic<u32> count{0};
};
DiscoveryRequestSlot g_discoveryRequests[kDiscoveryRequestEntries];
std::atomic<u32> g_discoveryRequestVersion{0};
std::atomic<u32> g_discoveryPendingCount{0};

struct RecentUnknownSkeletons {
    uptr segments[8]{};
    u32 cursor = 0u;
};
thread_local RecentUnknownSkeletons g_recentUnknownSkeletons;

struct PreserveScratch {
    
    
    std::vector<Matrix> matrices;
};
thread_local PreserveScratch g_preserveScratch;

u16 ReadU16(const void* p) {
    const auto* b = static_cast<const u8*>(p);
    return static_cast<u16>(b[0] | (static_cast<u16>(b[1]) << 8));
}

u32 ReadU32(const void* p) {
    const auto* b = static_cast<const u8*>(p);
    return static_cast<u32>(b[0]) |
           (static_cast<u32>(b[1]) << 8) |
           (static_cast<u32>(b[2]) << 16) |
           (static_cast<u32>(b[3]) << 24);
}

i32 ReadI32(const void* p) { return static_cast<i32>(ReadU32(p)); }

bool BytesEqual(const void* a, const u8* b, std::size_t n) {
    return std::memcmp(a, b, n) == 0;
}

void WriteAbsoluteJump(u8* dst, const void* target) {
    
    dst[0] = 0xFF; dst[1] = 0x25;
    dst[2] = 0; dst[3] = 0; dst[4] = 0; dst[5] = 0;
    const u64 value = reinterpret_cast<u64>(target);
    std::memcpy(dst + 6, &value, sizeof(value));
}

bool InstallDetour(void* target, void* replacement, u32 patchLen, void** trampolineOut) {
    if (!target || !replacement || !trampolineOut || patchLen < 14u) return false;

    auto* trampoline = static_cast<u8*>(VirtualAlloc(nullptr, patchLen + 14u,
        MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;

    std::memcpy(trampoline, target, patchLen);
    WriteAbsoluteJump(trampoline + patchLen, static_cast<u8*>(target) + patchLen);

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, patchLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    WriteAbsoluteJump(static_cast<u8*>(target), replacement);
    for (u32 i = 14; i < patchLen; ++i) static_cast<u8*>(target)[i] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), target, patchLen);
    DWORD ignored = 0;
    VirtualProtect(target, patchLen, oldProtect, &ignored);
    *trampolineOut = trampoline;
    return true;
}

bool NopVerifiedBytes(void* target, const u8* expected, u32 count) {
    if (!target || !expected || !count || !BytesEqual(target, expected, count)) return false;
    DWORD oldProtect = 0;
    if (!VirtualProtect(target, count, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    std::memset(target, 0x90, count);
    FlushInstructionCache(GetCurrentProcess(), target, count);
    DWORD ignored = 0;
    VirtualProtect(target, count, oldProtect, &ignored);
    return true;
}

Vec4 ReadVec4(const u8* p) { return *reinterpret_cast<const Vec4*>(p); }
Vec4 Add(const Vec4& a, const Vec4& b) { return {a.x+b.x, a.y+b.y, a.z+b.z, a.w+b.w}; }

Vec4 RotateVectorRowMajor(const Vec4& v, const Matrix& m) {
    return {
        v.x * m.row[0].x + v.y * m.row[1].x + v.z * m.row[2].x,
        v.x * m.row[0].y + v.y * m.row[1].y + v.z * m.row[2].y,
        v.x * m.row[0].z + v.y * m.row[1].z + v.z * m.row[2].z,
        0.0f
    };
}

Vec4 InverseRotateVectorRowMajor(const Vec4& v, const Matrix& m) {
    
    
    
    return {
        v.x * m.row[0].x + v.y * m.row[0].y + v.z * m.row[0].z,
        v.x * m.row[1].x + v.y * m.row[1].y + v.z * m.row[1].z,
        v.x * m.row[2].x + v.y * m.row[2].y + v.z * m.row[2].z,
        0.0f
    };
}

Vec4 LocalOffsetFromAbsolutePose(const Matrix* pose, const PreserveBoneOp& op) {
    const Matrix& parent = pose[op.parent];
    const Vec4& childPos = pose[op.bone].row[3];
    const Vec4 parentToChild{
        childPos.x - parent.row[3].x,
        childPos.y - parent.row[3].y,
        childPos.z - parent.row[3].z,
        0.0f
    };
    return InverseRotateVectorRowMajor(parentToChild, parent);
}

bool PoseAlreadyMatchesModelSkeleton(const PreserveModelCache* cache,
                                    const Matrix* source, u32 numBones) {
    if (!cache || !source || cache->count != numBones) return false;

    
    
    
    constexpr float kPositionTolerance = 0.05f;
    constexpr float kPositionToleranceSq = kPositionTolerance * kPositionTolerance;

    for (const PreserveBoneOp& op : cache->ops) {
        if (op.bone >= numBones || op.parent >= op.bone) return false;

        const Matrix& parentMatrix = source[op.parent];
        const Vec4 rotatedOffset = RotateVectorRowMajor(op.localOffset, parentMatrix);
        const Vec4& nativePos = source[op.bone].row[3];

        const float expectedX = parentMatrix.row[3].x + rotatedOffset.x;
        const float expectedY = parentMatrix.row[3].y + rotatedOffset.y;
        const float expectedZ = parentMatrix.row[3].z + rotatedOffset.z;
        const float dx = nativePos.x - expectedX;
        const float dy = nativePos.y - expectedY;
        const float dz = nativePos.z - expectedZ;
        if ((dx * dx + dy * dy + dz * dz) > kPositionToleranceSq)
            return false;
    }
    return true;
}

bool NativeBoneHasAnimatedTranslation(const Matrix* current, const Matrix* previous,
                                      const PreserveBoneOp& op) {
    if (!current || !previous) return false;

    
    
    
    
    
    constexpr float kTranslationMotionTolerance = 0.10f;
    constexpr float kTranslationMotionToleranceSq =
        kTranslationMotionTolerance * kTranslationMotionTolerance;
    const Vec4 currentLocal = LocalOffsetFromAbsolutePose(current, op);
    const Vec4 previousLocal = LocalOffsetFromAbsolutePose(previous, op);
    const float dx = currentLocal.x - previousLocal.x;
    const float dy = currentLocal.y - previousLocal.y;
    const float dz = currentLocal.z - previousLocal.z;
    return (dx * dx + dy * dy + dz * dz) > kTranslationMotionToleranceSq;
}

bool NativeTranslationLatched(const PreserveModelCache* cache, u32 bone) {
    if (!cache || !cache->translatedBoneWords || bone >= cache->count) return false;
    const u32 word = bone >> 6u;
    if (word >= cache->translatedBoneWordCount) return false;
    const u64 bit = 1ull << (bone & 63u);
    return (cache->translatedBoneWords[word].load(std::memory_order_relaxed) & bit) != 0u;
}

void LatchNativeTranslation(const PreserveModelCache* cache, u32 bone) {
    if (!cache || !cache->translatedBoneWords || bone >= cache->count) return;
    const u32 word = bone >> 6u;
    if (word >= cache->translatedBoneWordCount) return;
    const u64 bit = 1ull << (bone & 63u);
    cache->translatedBoneWords[word].fetch_or(bit, std::memory_order_relaxed);
}

bool RebuildPositionsFromModelSkeleton(const PreserveModelCache* cache,
                                       const Matrix* source, const Matrix* previous,
                                       u32 numBones, PreserveScratch& scratch,
                                       const Matrix*& outCurrent, const Matrix*& outPrevious) {
    outCurrent = source;
    outPrevious = previous;
    if (!cache || !source || numBones < 2u || numBones > kMaxModelBones ||
        cache->count != numBones)
        return false;

    const std::size_t matrixCount =
        static_cast<std::size_t>(numBones) * (previous ? 2u : 1u);
    try {
        if (scratch.matrices.size() != matrixCount)
            scratch.matrices.resize(matrixCount);
    } catch (...) {
        return false;
    }

    Matrix* current = scratch.matrices.data();
    Matrix* prevOut = previous ? (current + numBones) : nullptr;
    std::memcpy(current, source, static_cast<std::size_t>(numBones) * sizeof(Matrix));
    if (previous)
        std::memcpy(prevOut, previous, static_cast<std::size_t>(numBones) * sizeof(Matrix));

    
    
    
    
    
    
    for (const PreserveBoneOp& op : cache->ops) {
        if (op.bone >= numBones || op.parent >= op.bone)
            return false;

        bool keepNativeTranslation = NativeTranslationLatched(cache, op.bone);
        if (!keepNativeTranslation && previous &&
            NativeBoneHasAnimatedTranslation(source, previous, op)) {
            LatchNativeTranslation(cache, op.bone);
            keepNativeTranslation = true;
        }

        if (!keepNativeTranslation) {
            const Matrix& parentMatrix = current[op.parent];
            const Vec4 rotatedOffset = RotateVectorRowMajor(op.localOffset, parentMatrix);
            Matrix& child = current[op.bone];
            child.row[3].x = parentMatrix.row[3].x + rotatedOffset.x;
            child.row[3].y = parentMatrix.row[3].y + rotatedOffset.y;
            child.row[3].z = parentMatrix.row[3].z + rotatedOffset.z;
        }

        if (prevOut && !keepNativeTranslation) {
            const Matrix& parentMatrix = prevOut[op.parent];
            const Vec4 rotatedOffset = RotateVectorRowMajor(op.localOffset, parentMatrix);
            Matrix& child = prevOut[op.bone];
            child.row[3].x = parentMatrix.row[3].x + rotatedOffset.x;
            child.row[3].y = parentMatrix.row[3].y + rotatedOffset.y;
            child.row[3].z = parentMatrix.row[3].z + rotatedOffset.z;
        }
    }

    outCurrent = current;
    outPrevious = prevOut;
    return true;
}

u64 HashBytesFNV1a(u64 hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const u8*>(data);
    constexpr u64 kPrime = 1099511628211ull;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<u64>(bytes[i]);
        hash *= kPrime;
    }
    return hash;
}

u64 ComputePreserveSkeletonSignature(const SegmentList* list, u32 segmentCount) {
    if (!list || !list->segments || segmentCount < 2u || segmentCount > kMaxModelBones)
        return 0u;

    
    
    
    constexpr u64 kOffset = 14695981039346656037ull;
    constexpr u32 stride = 0x50u;
    constexpr u32 posOff = 0x20u;
    constexpr u32 parentOff = 0x38u;

    u64 hash = kOffset;
    hash = HashBytesFNV1a(hash, &segmentCount, sizeof(segmentCount));
    for (u32 bone = 0u; bone < segmentCount; ++bone) {
        const u8* seg = list->segments + static_cast<std::size_t>(bone) * stride;
        hash = HashBytesFNV1a(hash, seg + posOff, sizeof(Vec4));
        hash = HashBytesFNV1a(hash, seg + parentOff, sizeof(i32));
    }
    return hash;
}

u64 ComputePreserveQuickSkeletonSignature(const SegmentList* list, u32 segmentCount) {
    if (!list || !list->segments || segmentCount < 2u || segmentCount > kMaxModelBones)
        return 0u;

    
    
    
    constexpr u64 kOffset = 14695981039346656037ull;
    constexpr u32 stride = 0x50u;
    constexpr u32 posOff = 0x20u;
    constexpr u32 parentOff = 0x38u;
    constexpr u32 kSamples = 16u;

    u64 hash = HashBytesFNV1a(kOffset, &segmentCount, sizeof(segmentCount));
    const u32 samples = std::min<u32>(segmentCount, kSamples);
    for (u32 i = 0u; i < samples; ++i) {
        const u32 bone = (samples <= 1u) ? 0u
            : static_cast<u32>((static_cast<u64>(i) * (segmentCount - 1u)) / (samples - 1u));
        const u8* seg = list->segments + static_cast<std::size_t>(bone) * stride;
        hash = HashBytesFNV1a(hash, seg + posOff, sizeof(Vec4));
        hash = HashBytesFNV1a(hash, seg + parentOff, sizeof(i32));
    }
    return hash;
}

u64 ComputeBoneMapSignature(const void* boneMapPairs, u32 boneMapPairCount) {
    if (!boneMapPairs || boneMapPairCount == 0u || boneMapPairCount > kMaxModelBones)
        return 0u;
    constexpr u64 kOffset = 14695981039346656037ull;
    u64 hash = HashBytesFNV1a(kOffset, &boneMapPairCount, sizeof(boneMapPairCount));
    return HashBytesFNV1a(
        hash, boneMapPairs, static_cast<std::size_t>(boneMapPairCount) * 6u);
}

u32 PreserveCacheIndex(uptr segments) {
    
    
    u64 x = static_cast<u64>(segments >> 4u);
    x ^= x >> 17u;
    x *= 0x9E3779B185EBCA87ull;
    x ^= x >> 29u;
    return static_cast<u32>(x) & (kPreserveCacheTableEntries - 1u);
}

u32 DiscoveryRequestIndex(uptr segments) {
    u64 x = static_cast<u64>(segments >> 4u);
    x ^= x >> 17u;
    x *= 0x9E3779B185EBCA87ull;
    x ^= x >> 29u;
    return static_cast<u32>(x) & (kDiscoveryRequestEntries - 1u);
}

bool DiscoveryRequestContains(uptr segments, u32 count) {
    if (!segments || count < 2u || count > kMaxModelBones) return false;
    u32 index = DiscoveryRequestIndex(segments);
    for (u32 probe = 0; probe < kDiscoveryRequestEntries; ++probe) {
        const uptr slotSegments =
            g_discoveryRequests[index].segments.load(std::memory_order_acquire);
        if (slotSegments == 0u) return false;
        if (slotSegments == segments) {
            const u32 slotCount =
                g_discoveryRequests[index].count.load(std::memory_order_acquire);
            return slotCount == count;
        }
        index = (index + 1u) & (kDiscoveryRequestEntries - 1u);
    }
    return false;
}

void ResolveDiscoveryRequest(uptr segments, u32 count) {
    if (!segments) return;
    u32 index = DiscoveryRequestIndex(segments);
    for (u32 probe = 0; probe < kDiscoveryRequestEntries; ++probe) {
        uptr slotSegments =
            g_discoveryRequests[index].segments.load(std::memory_order_acquire);
        if (slotSegments == 0u) return;
        if (slotSegments == segments &&
            g_discoveryRequests[index].count.load(std::memory_order_acquire) == count) {
            g_discoveryRequests[index].count.store(0u, std::memory_order_relaxed);
            g_discoveryRequests[index].segments.store(
                kDiscoveryTombstone, std::memory_order_release);
            const u32 pending = g_discoveryPendingCount.load(std::memory_order_relaxed);
            if (pending > 0u)
                g_discoveryPendingCount.fetch_sub(1u, std::memory_order_release);
            return;
        }
        index = (index + 1u) & (kDiscoveryRequestEntries - 1u);
    }
}

void RequestUncachedModelDiscovery(const SegmentList* list, u32 numBones) {
    if (!list || !list->segments || list->count != numBones ||
        numBones < 2u || numBones > kMaxModelBones)
        return;

    const uptr segments = reinterpret_cast<uptr>(list->segments);

    
    
    
    for (uptr recent : g_recentUnknownSkeletons.segments) {
        if (recent == segments) return;
    }
    g_recentUnknownSkeletons.segments[
        g_recentUnknownSkeletons.cursor++ & 7u] = segments;

    u32 index = DiscoveryRequestIndex(segments);
    u32 reusable = kDiscoveryRequestEntries;
    for (u32 probe = 0; probe < kDiscoveryRequestEntries; ++probe) {
        auto& slot = g_discoveryRequests[index];
        uptr slotSegments = slot.segments.load(std::memory_order_acquire);
        if (slotSegments == segments) return;
        if (slotSegments == kDiscoveryTombstone && reusable == kDiscoveryRequestEntries)
            reusable = index;
        if (slotSegments == 0u) {
            if (reusable == kDiscoveryRequestEntries) reusable = index;
            break;
        }
        index = (index + 1u) & (kDiscoveryRequestEntries - 1u);
    }
    if (reusable == kDiscoveryRequestEntries) return;

    auto& target = g_discoveryRequests[reusable];
    uptr expected = target.segments.load(std::memory_order_relaxed);
    if (expected != 0u && expected != kDiscoveryTombstone) return;
    if (!target.segments.compare_exchange_strong(
            expected, segments, std::memory_order_acq_rel, std::memory_order_relaxed))
        return;

    
    
    g_discoveryPendingCount.fetch_add(1u, std::memory_order_relaxed);
    target.count.store(numBones, std::memory_order_release);
    g_discoveryRequestVersion.fetch_add(1u, std::memory_order_release);
}

const PreserveModelCache* FindCachedModel(uptr segments, u32 count) {
    if (!segments || count < 2u || count > kMaxModelBones)
        return nullptr;

    u32 index = PreserveCacheIndex(segments);
    for (u32 probe = 0; probe < kPreserveCacheTableEntries; ++probe) {
        const PreserveModelCache* cache =
            g_preserveCacheTable[index].load(std::memory_order_acquire);
        if (!cache)
            return nullptr; 
        if (cache->segments == segments && cache->count == count)
            return cache;
        index = (index + 1u) & (kPreserveCacheTableEntries - 1u);
    }
    return nullptr;
}

const PreserveModelCache* MatchCachedSkeleton(const SegmentList* list, u32 numBones,
                                               u32& matchKind) {
    matchKind = 0u;
    if (!list || !list->segments || list->count != numBones)
        return nullptr;

    const PreserveModelCache* cache =
        FindCachedModel(reinterpret_cast<uptr>(list->segments), numBones);
    if (!cache)
        return nullptr;

    matchKind = (reinterpret_cast<uptr>(list) == cache->segmentList) ? 1u : 2u;
    return cache;
}

Matrix MakeSkinningMatrix(const Matrix& s, const Vec4& p) {
    Matrix o{};
    o.row[0] = {s.row[0].x, s.row[0].y, s.row[0].z, 0.0f};
    o.row[1] = {s.row[1].x, s.row[1].y, s.row[1].z, 0.0f};
    o.row[2] = {s.row[2].x, s.row[2].y, s.row[2].z, 0.0f};
    o.row[3].x = s.row[3].x - p.x*s.row[0].x - p.y*s.row[1].x - p.z*s.row[2].x;
    o.row[3].y = s.row[3].y - p.x*s.row[0].y - p.y*s.row[1].y - p.z*s.row[2].y;
    o.row[3].z = s.row[3].z - p.x*s.row[0].z - p.y*s.row[1].z - p.z*s.row[2].z;
    o.row[3].w = 1.0f;
    return o;
}

void CalcHook(const Matrix* root, const SegmentList* list, const Matrix* matrices,
              const Matrix* prev, u32 numBones, void* state) {
    const u32 segmentCount = list ? list->count : 0u;

    const Matrix* calcMatrices = matrices;
    const Matrix* calcPrev = prev;

    u32 preserveMatchKind = 0u;
    const PreserveModelCache* preserveCache = nullptr;
    if (g_preserveModelBonePositions.load(std::memory_order_relaxed) &&
        g_cutsceneActive.load(std::memory_order_acquire) &&
        matrices && numBones >= 2u && numBones <= kMaxModelBones) {
        preserveCache = MatchCachedSkeleton(list, numBones, preserveMatchKind);
        if (!preserveCache && numBones >= kMinAutoDiscoveredModelBones)
            RequestUncachedModelDiscovery(list, numBones);
    }

    if (preserveCache) {
        
        
        
        const bool nativePoseAlreadyCompatible = !preserveCache->correct ||
            PoseAlreadyMatchesModelSkeleton(preserveCache, matrices, numBones);

        if (!nativePoseAlreadyCompatible) {
            const Matrix* rebuiltCurrent = matrices;
            const Matrix* rebuiltPrevious = prev;
            if (RebuildPositionsFromModelSkeleton(
                    preserveCache, matrices, prev, numBones, g_preserveScratch,
                    rebuiltCurrent, rebuiltPrevious)) {
                calcMatrices = rebuiltCurrent;
                calcPrev = rebuiltPrevious;
            }
        }

        
        
        
        if (!g_preserveMatched.load(std::memory_order_relaxed) &&
            !g_preserveMatched.exchange(true, std::memory_order_acq_rel)) {
            const uptr ret = reinterpret_cast<uptr>(_ReturnAddress());
            const uptr base = GameBuild::Base();
            const uptr callerRva = (ret >= base) ? (ret - base) : ret;
            g_preserveLastCallerRva.store(callerRva, std::memory_order_relaxed);
            g_preserveLastBoneCount.store(numBones, std::memory_order_relaxed);
            g_preserveLastMatchKind.store(preserveMatchKind, std::memory_order_relaxed);

            const Vec4 rootPos = matrices[0].row[3];
            Log::Detailf(
                "Cutscene skeleton: matched cached model skeleton via %s backing=%p bones=%u callerRVA=0x%llX nativeRoot=(%.3f, %.3f, %.3f) initialPose=%s",
                preserveMatchKind == 1u ? "exact SegmentList" : "shared segment backing",
                list ? list->segments : nullptr, numBones,
                static_cast<unsigned long long>(callerRva),
                rootPos.x, rootPos.y, rootPos.z,
                !preserveCache->correct
                    ? "left-as-placed (no DRAWABLE component)"
                    : (nativePoseAlreadyCompatible ? "already-model-compatible" : "corrected"));
        }
    }

    const bool extended = numBones > kStockMaxBones || segmentCount > kStockMaxBones;
    if (!extended) {
        g_originalCalc(root, list, calcMatrices, calcPrev, numBones, state);
        return;
    }

    if (!root || !list || !calcMatrices || !state || !list->segments ||
        numBones < 2u || numBones > kMaxModelBones) {
        if (!g_loggedInvalidCalc.exchange(true))
            Log::Write("8191 patch: rejected invalid extended CalcSkeletonMatrices call");
        return;
    }

    u32 stateCapacity = 0u;
    auto* backing = *reinterpret_cast<u8**>(static_cast<u8*>(state) + 0x8u);
    if (backing) stateCapacity = ReadU32(backing) & 0x7FFFFFFFu;
    if (stateCapacity != 0u && stateCapacity < numBones) {
        if (!g_loggedInvalidCalc.exchange(true))
            Log::Write("8191 patch: MatrixState capacity is smaller than model bone count; update blocked");
        return;
    }

    HANDLE heap = GetProcessHeap();
    auto* absPos = static_cast<Vec4*>(HeapAlloc(heap, HEAP_ZERO_MEMORY,
        static_cast<std::size_t>(numBones) * sizeof(Vec4)));
    auto* current = static_cast<Matrix*>(HeapAlloc(heap, HEAP_ZERO_MEMORY,
        static_cast<std::size_t>(numBones - 1u) * sizeof(Matrix)));
    auto* previous = static_cast<Matrix*>(HeapAlloc(heap, HEAP_ZERO_MEMORY,
        static_cast<std::size_t>(numBones - 1u) * sizeof(Matrix)));

    if (!absPos || !current || !previous) {
        if (absPos) HeapFree(heap, 0, absPos);
        if (current) HeapFree(heap, 0, current);
        if (previous) HeapFree(heap, 0, previous);
        Log::Write("8191 patch: CalcSkeletonMatrices allocation failure");
        return;
    }

    const u32 processed = (segmentCount < numBones) ? segmentCount : numBones;
    constexpr u32 stride = 0x50u;
    constexpr u32 posOff = 0x20u;
    constexpr u32 parentOff = 0x38u;

    if (processed > 0u) absPos[0] = ReadVec4(list->segments + posOff);
    bool valid = true;
    for (u32 bone = 1; bone < processed; ++bone) {
        const u8* seg = list->segments + static_cast<std::size_t>(bone) * stride;
        Vec4 pos = ReadVec4(seg + posOff);
        const i32 parent = ReadI32(seg + parentOff);
        if (parent >= 0) {
            if (static_cast<u32>(parent) >= bone) { valid = false; break; }
            pos = Add(pos, absPos[static_cast<u32>(parent)]);
        }
        absPos[bone] = pos;
        current[bone - 1u] = MakeSkinningMatrix(calcMatrices[bone], pos);
        previous[bone - 1u] = calcPrev ? MakeSkinningMatrix(calcPrev[bone], pos) : current[bone - 1u];
    }

    if (valid) {
        const u32 firstMissing = processed > 1u ? processed : 1u;
        for (u32 bone = firstMissing; bone < numBones; ++bone) {
            current[bone - 1u] = *root;
            previous[bone - 1u] = *root;
        }
        g_setMatrices(state, current, 1u, numBones - 1u);
        g_setPrevMatrices(state, previous, 1u, numBones - 1u);
    } else if (!g_loggedInvalidCalc.exchange(true)) {
        Log::Write("8191 patch: invalid parent ordering in extended skeleton");
    }

    HeapFree(heap, 0, previous);
    HeapFree(heap, 0, current);
    HeapFree(heap, 0, absPos);
}

i32 ObjectZoneNumSegmentsHook(void* zone) {
    const i32 raw = g_originalObjectZoneNumSegments ? g_originalObjectZoneNumSegments(zone) : 0;
    if (raw <= static_cast<i32>(kStockMaxBones)) return raw;

    const uptr ret = reinterpret_cast<uptr>(_ReturnAddress());
    const uptr base = GameBuild::Base();
    const uptr callerRva = (ret >= base) ? (ret - base) : ret;
    if (callerRva == kCallerDynamicHairScratch) {
        if (!g_loggedDynamicHairClamp.exchange(true))
            Log::Detail("8191 patch: DynamicHair legacy segment path clamped to 256");
        return static_cast<i32>(kStockMaxBones);
    }
    return raw;
}

u32* UsageHook(void* instance) {
    u32* stock = g_originalUsage(instance);
    if (!stock) return nullptr;

    auto* model = reinterpret_cast<u8*>(stock) - 0x68u;
    const u16 numBones = ReadU16(model + kModelNumBonesOffset);
    if (numBones <= kStockMaxBones) return stock;
    if (numBones > kMaxModelBones) return stock;

    for (auto& entry : g_usageCache) {
        if (entry.state.load(std::memory_order_acquire) != 2u ||
            entry.model != model || entry.numBones != numBones) continue;
        bool same = true;
        for (u32 w = 0; w < 8u; ++w) {
            if (entry.stockWords[w] != stock[w]) { same = false; break; }
        }
        if (same) return entry.words;
    }

    for (auto& entry : g_usageCache) {
        u32 expected = 0u;
        if (!entry.state.compare_exchange_strong(expected, 1u, std::memory_order_acq_rel)) continue;

        entry.model = model;
        entry.numBones = numBones;
        std::memset(entry.words, 0, sizeof(entry.words));
        for (u32 w = 0; w < 8u; ++w) {
            entry.stockWords[w] = stock[w];
            entry.words[w] = stock[w];
        }
        for (u32 bone = 256u; bone < static_cast<u32>(numBones); ++bone)
            entry.words[bone >> 5] |= 1u << (bone & 31u);

        entry.state.store(2u, std::memory_order_release);
        if (!g_loggedUsageMap.exchange(true))
            Log::Detail("8191 patch: synthetic >256-bone usage map active");
        return entry.words;
    }

    Log::Write("8191 patch: usage-map cache exhausted; falling back to stock map");
    return stock;
}
} 

namespace BonePatch {
bool Install() {
    if (g_installed.load()) return true;
    if (!GameBuild::IsSupportedRetail()) {
        Log::Write("8191 patch: unsupported ROTTR.exe build");
        return false;
    }

    const uptr base = GameBuild::Base();
    void* calc = reinterpret_cast<void*>(base + kRvaCalcSkeletonMatrices);
    void* usage = reinterpret_cast<void*>(base + kRvaGetBoneUsageMap);
    void* numSegments = reinterpret_cast<void*>(base + kRvaObjectZoneNumSegments);

    if (!BytesEqual(calc, kCalcPrologue, sizeof(kCalcPrologue)) ||
        !BytesEqual(usage, kUsageMapPrologue, sizeof(kUsageMapPrologue)) ||
        !BytesEqual(numSegments, kObjectZoneNumSegmentsPrologue, sizeof(kObjectZoneNumSegmentsPrologue)) ||
        !BytesEqual(reinterpret_cast<void*>(base + kRvaDrawableBoneConstantsClamp),
                    kDrawableBoneConstantsClamp, sizeof(kDrawableBoneConstantsClamp)) ||
        !BytesEqual(reinterpret_cast<void*>(base + kRvaInstanceBoneConstantsClamp),
                    kInstanceBoneConstantsClamp, sizeof(kInstanceBoneConstantsClamp))) {
        Log::Write("8191 patch: target signature mismatch; patch not installed");
        return false;
    }

    if (!NopVerifiedBytes(reinterpret_cast<void*>(base + kRvaDrawableBoneConstantsClamp),
                          kDrawableBoneConstantsClamp, sizeof(kDrawableBoneConstantsClamp)) ||
        !NopVerifiedBytes(reinterpret_cast<void*>(base + kRvaInstanceBoneConstantsClamp),
                          kInstanceBoneConstantsClamp, sizeof(kInstanceBoneConstantsClamp))) {
        Log::Write("8191 patch: failed to remove bone-constant allocation clamps");
        return false;
    }

    g_setMatrices = reinterpret_cast<SetMatricesFn>(base + kRvaSetMatrices);
    g_setPrevMatrices = reinterpret_cast<SetMatricesFn>(base + kRvaSetPrevMatrices);

    void* trampoline = nullptr;
    if (!InstallDetour(calc, reinterpret_cast<void*>(&CalcHook), sizeof(kCalcPrologue), &trampoline)) {
        Log::Write("8191 patch: failed to hook CalcSkeletonMatrices");
        return false;
    }
    g_originalCalc = reinterpret_cast<CalcFn>(trampoline);

    trampoline = nullptr;
    if (!InstallDetour(numSegments, reinterpret_cast<void*>(&ObjectZoneNumSegmentsHook),
                       sizeof(kObjectZoneNumSegmentsPrologue), &trampoline)) {
        Log::Write("8191 patch: failed to hook ObjectZone::NumSegments");
        return false;
    }
    g_originalObjectZoneNumSegments = reinterpret_cast<NumSegmentsFn>(trampoline);

    trampoline = nullptr;
    if (!InstallDetour(usage, reinterpret_cast<void*>(&UsageHook), sizeof(kUsageMapPrologue), &trampoline)) {
        Log::Write("8191 patch: failed to hook GetBoneUsageMap");
        return false;
    }
    g_originalUsage = reinterpret_cast<UsageFn>(trampoline);

    g_installed.store(true);
    Log::Detail("8191 patch: installed successfully (integrated framework module)");
    return true;
}

bool Installed() { return g_installed.load(); }

bool PreserveModelBonePositionsEnabled() {
    return g_preserveModelBonePositions.load(std::memory_order_relaxed);
}

bool CutsceneActive() {
    return g_cutsceneActive.load(std::memory_order_acquire);
}

void SetCutsceneActive(bool value) {
    const bool previous = g_cutsceneActive.exchange(value, std::memory_order_acq_rel);
    if (previous == value ||
        !g_preserveModelBonePositions.load(std::memory_order_relaxed))
        return;
    Log::Detail(value
        ? "Cutscene skeleton: cinematic gate active"
        : "Cutscene skeleton: cinematic gate inactive");
}

void SetPreserveModelBonePositionsEnabled(bool value) {
    const bool previous = g_preserveModelBonePositions.exchange(value, std::memory_order_acq_rel);
    if (!value)
        g_cutsceneActive.store(false, std::memory_order_release);
    if (previous == value) return;

    g_preserveMatched.store(false, std::memory_order_relaxed);
    g_preserveLastCallerRva.store(0u, std::memory_order_relaxed);
    g_preserveLastBoneCount.store(0u, std::memory_order_relaxed);
    g_preserveLastMatchKind.store(0u, std::memory_order_relaxed);
    Log::Detail(value
        ? "Cutscene skeleton: bone-position fix armed; correction is gated to the active cinematic camera stack"
        : "Cutscene skeleton: bone-position fix disabled");
}

bool ModelSegmentListCached(const void* segmentList, unsigned int segmentCount,
                            bool requireBoneMap) {
    const auto* typed = static_cast<const SegmentList*>(segmentList);
    if (!typed || !typed->segments || segmentCount < 2u || segmentCount > kMaxModelBones)
        return false;
    const PreserveModelCache* cache = FindCachedModel(
        reinterpret_cast<uptr>(typed->segments), segmentCount);
    return cache && (!requireBoneMap || cache->haveBoneMap);
}

bool ModelSegmentListQuickSignatureMatches(const void* segmentList, unsigned int segmentCount) {
    const auto* typed = static_cast<const SegmentList*>(segmentList);
    if (!typed || !typed->segments || segmentCount < 2u || segmentCount > kMaxModelBones)
        return false;
    const PreserveModelCache* cache = FindCachedModel(
        reinterpret_cast<uptr>(typed->segments), segmentCount);
    if (!cache || cache->quickSkeletonSignature == 0u) return false;
    return cache->quickSkeletonSignature ==
        ComputePreserveQuickSkeletonSignature(typed, segmentCount);
}

void RegisterModelSegmentList(const void* segmentList, unsigned int segmentCount,
                              const void* boneMapPairs, unsigned int boneMapPairCount,
                              bool allowMissingBoneMap, bool correct) {
    const auto* typed = static_cast<const SegmentList*>(segmentList);
    if (!typed || !typed->segments || segmentCount < 2u || segmentCount > kMaxModelBones)
        return;

    const uptr newList = reinterpret_cast<uptr>(segmentList);
    const uptr newSegments = reinterpret_cast<uptr>(typed->segments);
    const uptr newBoneMapPairs = reinterpret_cast<uptr>(boneMapPairs);
    const bool haveBoneMap =
        boneMapPairs && boneMapPairCount > 0u && boneMapPairCount <= kMaxModelBones;
    if (!haveBoneMap && !allowMissingBoneMap)
        return;

    
    
    
    
    
    const u64 quickSkeletonSignature = ComputePreserveQuickSkeletonSignature(typed, segmentCount);
    const u64 skeletonSignature = ComputePreserveSkeletonSignature(typed, segmentCount);
    const u64 boneMapSignature = ComputeBoneMapSignature(boneMapPairs, boneMapPairCount);
    const PreserveModelCache* existing = FindCachedModel(newSegments, segmentCount);
    const bool sameSkeleton = existing && existing->skeletonSignature == skeletonSignature;
    const bool samePriority = existing && existing->priorityPlayer == allowMissingBoneMap;
    const bool mapCanReuse = existing && (
        (existing->haveBoneMap && !haveBoneMap) ||
        (existing->haveBoneMap == haveBoneMap &&
         (!haveBoneMap || existing->boneMapSignature == boneMapSignature)));
    // (a skeleton shared by objects with and without DRAWABLE: corrected once any of them has it)
    if (existing && sameSkeleton && existing->correct && !correct) correct = true;
    const bool sameCorrection = existing && existing->correct == correct;
    if (sameSkeleton && samePriority && mapCanReuse && sameCorrection)
        return;

    const bool reusedBackingWithChangedSkeleton = existing && !sameSkeleton;

    auto cache = std::unique_ptr<PreserveModelCache>(new (std::nothrow) PreserveModelCache());
    if (!cache) {
        Log::Write("Cutscene skeleton: failed to allocate cached model hierarchy");
        return;
    }

    cache->segmentList = newList;
    cache->segments = newSegments;
    cache->boneMapPairs = newBoneMapPairs;
    cache->count = segmentCount;
    cache->boneMapPairCount = boneMapPairCount;
    cache->haveBoneMap = haveBoneMap;
    cache->priorityPlayer = allowMissingBoneMap;
    cache->correct = correct;
    cache->skeletonSignature = skeletonSignature;
    cache->quickSkeletonSignature = quickSkeletonSignature;
    cache->boneMapSignature = boneMapSignature;
    try {
        cache->ops.reserve(segmentCount > 2u ? segmentCount - 2u : 0u);
    } catch (...) {
        Log::Write("Cutscene skeleton: failed to reserve cached hierarchy");
        return;
    }
    cache->translatedBoneWordCount = (segmentCount + 63u) >> 6u;
    cache->translatedBoneWords.reset(new (std::nothrow) std::atomic<u64>[cache->translatedBoneWordCount]);
    if (!cache->translatedBoneWords) {
        Log::Write("Cutscene skeleton: failed to allocate translated-bone mask");
        return;
    }
    for (u32 i = 0u; i < cache->translatedBoneWordCount; ++i)
        cache->translatedBoneWords[i].store(0u, std::memory_order_relaxed);

    constexpr u32 stride = 0x50u;
    constexpr u32 posOff = 0x20u;
    constexpr u32 parentOff = 0x38u;

    
    
    
    std::vector<u8> animatedBoneMask;
    std::vector<u32> globalBoneIds;
    if (haveBoneMap) {
        try {
            animatedBoneMask.assign(segmentCount, 0u);
            globalBoneIds.assign(segmentCount, 0xFFFFFFFFu);
        } catch (...) {
            Log::Write("Cutscene skeleton: failed to allocate animation-bone map");
            return;
        }

        const auto* pairs = static_cast<const u8*>(boneMapPairs);
        constexpr std::size_t pairStride = 6u;
        constexpr std::size_t localIndexOff = 4u;
        for (u32 i = 0; i < boneMapPairCount; ++i) {
            const u8* pair = pairs + static_cast<std::size_t>(i) * pairStride;
            const u32 globalId = ReadU32(pair);
            const u16 localIndex = ReadU16(pair + localIndexOff);
            if (localIndex < segmentCount) {
                globalBoneIds[localIndex] = globalId;
                if (!animatedBoneMask[localIndex]) {
                    animatedBoneMask[localIndex] = 1u;
                    ++cache->animatedBoneCount;
                }
            }
        }
        cache->physicsBoneCount = segmentCount - cache->animatedBoneCount;
        cache->animatedBoneMask.swap(animatedBoneMask);
        cache->globalBoneIds.swap(globalBoneIds);
    }

    // the animated bones below the root's children: put back at their model offsets from their posed parents (a
    // model without DRAWABLE gets none: left as the cutscene places it)
    if (correct) {
        for (u32 bone = 1u; bone < segmentCount; ++bone) {
            const u8* seg = typed->segments + static_cast<std::size_t>(bone) * stride;
            const i32 parent = ReadI32(seg + parentOff);
            if (parent <= 0)
                continue;
            if (haveBoneMap && !cache->animatedBoneMask[bone])
                continue;
            if (static_cast<u32>(parent) >= bone || static_cast<u32>(parent) >= segmentCount)
                return;   // (parents after children: not a hierarchy this can walk)
            PreserveBoneOp op{};
            op.bone = bone;
            op.parent = static_cast<u32>(parent);
            op.localOffset = ReadVec4(seg + posOff);
            cache->ops.push_back(op);
        }
    }

    // (a corrected model with nothing to correct: not cached; a left-alone one is, so it is not looked for again)
    if (cache->correct && cache->ops.empty())
        return;

    
    u32 index = PreserveCacheIndex(newSegments);
    u32 publishIndex = kPreserveCacheTableEntries;
    bool replacing = false;
    for (u32 probe = 0; probe < kPreserveCacheTableEntries; ++probe) {
        const PreserveModelCache* slot =
            g_preserveCacheTable[index].load(std::memory_order_acquire);
        if (!slot) {
            publishIndex = index;
            break;
        }
        if (slot->segments == newSegments && slot->count == segmentCount) {
            
            
            
            const bool slotSameSkeleton = slot->skeletonSignature == skeletonSignature;
            const bool slotSamePriority = slot->priorityPlayer == allowMissingBoneMap;
            const bool slotMapCanReuse =
                (slot->haveBoneMap && !haveBoneMap) ||
                (slot->haveBoneMap == haveBoneMap &&
                 (!haveBoneMap || slot->boneMapSignature == boneMapSignature));
            if (slotSameSkeleton && slotSamePriority && slotMapCanReuse && slot->correct == correct)
                return;
            publishIndex = index;
            replacing = true;
            break;
        }
        index = (index + 1u) & (kPreserveCacheTableEntries - 1u);
    }
    if (publishIndex == kPreserveCacheTableEntries) {
        Log::Write("Cutscene skeleton: model cache table is full; additional skeleton ignored");
        return;
    }

    const PreserveModelCache* published = cache.get();
    try {
        g_preserveOwnedCaches.emplace_back(std::move(cache));
    } catch (...) {
        Log::Write("Cutscene skeleton: failed to retain cached model hierarchy");
        return;
    }

    g_preserveCacheTable[publishIndex].store(published, std::memory_order_release);
    ResolveDiscoveryRequest(newSegments, segmentCount);
    if (reusedBackingWithChangedSkeleton) {
        Log::Detailf(
            "Cutscene skeleton: refreshed reused backing=%p bones=%u after model/outfit transform change",
            reinterpret_cast<void*>(newSegments), segmentCount);
    }
    if (!replacing) {
        const u32 cached = g_preserveCachedModelCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        
        
        if (cached <= 4u) {
            Log::Detailf(
                "Cutscene skeleton: cached model #%u backing=%p bones=%u correctedDescendants=%u physicsBonesPreserved=%u boneMap=%s",
                cached, reinterpret_cast<void*>(newSegments), segmentCount,
                static_cast<unsigned int>(published->ops.size()), published->physicsBoneCount,
                haveBoneMap ? (published->correct ? "yes" : "yes/left-as-placed (no DRAWABLE component)")
                    : "unavailable-fallback");
        }
    }
}

unsigned int CachedModelSkeletonCount() {
    return g_preserveCachedModelCount.load(std::memory_order_acquire);
}

unsigned int PendingModelSkeletonRequestVersion() {
    return g_discoveryRequestVersion.load(std::memory_order_acquire);
}

unsigned int PendingModelSkeletonRequestCount() {
    return g_discoveryPendingCount.load(std::memory_order_acquire);
}

bool ModelSegmentListDiscoveryRequested(const void* segmentList, unsigned int segmentCount) {
    const auto* typed = static_cast<const SegmentList*>(segmentList);
    if (!typed || !typed->segments) return false;
    return DiscoveryRequestContains(reinterpret_cast<uptr>(typed->segments), segmentCount);
}

bool PreserveModelBonePositionsMatched() {
    return g_preserveMatched.load(std::memory_order_relaxed);
}

unsigned long long PreserveModelBonePositionsLastCallerRva() {
    return static_cast<unsigned long long>(g_preserveLastCallerRva.load(std::memory_order_relaxed));
}

unsigned int PreserveModelBonePositionsLastBoneCount() {
    return g_preserveLastBoneCount.load(std::memory_order_relaxed);
}

unsigned int PreserveModelBonePositionsLastMatchKind() {
    return g_preserveLastMatchKind.load(std::memory_order_relaxed);
}
} 
