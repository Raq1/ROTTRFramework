#pragma once

namespace BonePatch {


bool Install();
bool Installed();







bool PreserveModelBonePositionsEnabled();
void SetPreserveModelBonePositionsEnabled(bool value);


bool CutsceneActive();
void SetCutsceneActive(bool value);
bool ModelSegmentListCached(const void* segmentList, unsigned int segmentCount,
                            bool requireBoneMap = false);


bool ModelSegmentListQuickSignatureMatches(const void* segmentList, unsigned int segmentCount);
// correct: the model's bones are put back at their model offsets in cutscenes (the player, and the objects with a
// DRAWABLE component: characters); false: cached but left as the cutscene places it (props).
void RegisterModelSegmentList(const void* segmentList, unsigned int segmentCount,
                              const void* boneMapPairs = nullptr,
                              unsigned int boneMapPairCount = 0u,
                              bool allowMissingBoneMap = false,
                              bool correct = true);
unsigned int CachedModelSkeletonCount();







unsigned int PendingModelSkeletonRequestVersion();
unsigned int PendingModelSkeletonRequestCount();
bool ModelSegmentListDiscoveryRequested(const void* segmentList, unsigned int segmentCount);



bool PreserveModelBonePositionsMatched();
unsigned long long PreserveModelBonePositionsLastCallerRva();
unsigned int PreserveModelBonePositionsLastBoneCount();
unsigned int PreserveModelBonePositionsLastMatchKind();


constexpr unsigned MaxModelBones() { return 8191u; }
constexpr unsigned MaxMeshPaletteBones() { return 256u; }
}
