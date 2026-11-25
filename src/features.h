#pragma once
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Features {
struct PlayerTransform {
    float position[3]{};
    float rotation[3]{};
};

bool CreateStartupHooks();
bool CreateHooks();
bool HooksCreated();





struct LoadingProfile {
    bool available{};
    bool active{};
    bool completed{};
    std::uint64_t sessionId{};
    double elapsedSeconds{};
    std::uint64_t worldLoadUnitCalls{};
    double worldLoadUnitAggregateSeconds{};
    double worldLoadUnitMaxMs{};
    std::uint64_t resolveDataCalls{};
    std::uint64_t resolveDataBytes{};
    double resolveDataAggregateSeconds{};
    double resolveDataMaxMs{};
    std::uint64_t resolveDoneCalls{};
    std::uint64_t processReadOperations{};
    std::uint64_t processReadBytes{};
    std::uint64_t processWriteBytes{};
    double processUserCpuSeconds{};
    double processKernelCpuSeconds{};
    double cpuCoreEquivalent{};

    
    
    
    bool fastLoadingEnabledForSession{};
    std::uint64_t fastLoadingSleepShortenedCalls{};
    std::uint64_t fastLoadingSleepRequestedMs{};
    std::uint64_t loadingPresentCalls{};
    std::uint64_t loadingPresentSync0Calls{};
    std::uint64_t loadingPresentSync1Calls{};
    std::uint64_t loadingPresentSyncOtherCalls{};
    std::uint64_t loadingPresentForcedNoSyncCalls{};
    double loadingPresentAggregateSeconds{};
    double loadingPresentMaxMs{};

    
    bool fastLoadingAggressiveRendererForSession{};
    int fastLoadingRendererOriginalSetting{-1};
    int fastLoadingRendererOriginalMode{-1};
    int fastLoadingRendererLastSetting{-1};
    int fastLoadingRendererLastMode{-1};
    std::uint64_t fastLoadingRendererBeginFrameCalls{};
    std::uint64_t fastLoadingRendererOverrideWrites{};
    std::uint64_t fastLoadingRendererRestoreWrites{};

    
    
    bool fastLoadingNonBlockingPresentForSession{};
    std::uint64_t loadingPresentDoNotWaitCalls{};
    std::uint64_t loadingPresentDoNotWaitSuccessCalls{};
    std::uint64_t loadingPresentWouldBlockDrops{};
    std::uint64_t loadingPresentOtherFailures{};

    bool readApiAvailable{};
    std::uint64_t win32ReadFileCalls{};
    std::uint64_t win32ReadFileSyncCalls{};
    std::uint64_t win32ReadFileOverlappedCalls{};
    std::uint64_t win32ReadFileExCalls{};
    std::uint64_t win32ReadRequestedBytes{};
    std::uint64_t win32ReadCompletedBytes{};
    double win32ReadAggregateSeconds{};
    double win32ReadMaxMs{};
    std::uint64_t win32ReadMaxConcurrent{};
    std::uint64_t win32ReadBucket4K{};
    std::uint64_t win32ReadBucket64K{};
    std::uint64_t win32ReadBucket256K{};
    std::uint64_t win32ReadBucket1M{};
    std::uint64_t win32ReadBucketLarge{};

    
    
    double loadingStateWallSeconds[6]{};
    
    double loadingStateTickSeconds[6]{};
    
    double loadingStateThreadCpuSeconds[6]{};
    std::uint64_t loadingStateTickCalls[6]{};
    double loadingStateFirstSeenSeconds[6]{};
    double loadingTickAggregateSeconds{};
    double loadingTickThreadCpuSeconds{};
    double loadingTickMaxMs{};

    
    
    std::uint64_t startLoadingHelperCalls{};
    double startLoadingHelperSeconds{};
    double startLoadingHelperThreadCpuSeconds{};
    double startLoadingHelperMaxMs{};
    std::uint64_t blockingLoadPumpCalls{};
    double blockingLoadPumpSeconds{};
    double blockingLoadPumpThreadCpuSeconds{};
    double blockingLoadPumpMaxMs{};
    std::uint64_t loadWorkPumpCalls{};
    double loadWorkPumpSeconds{};
    double loadWorkPumpThreadCpuSeconds{};
    double loadWorkPumpMaxMs{};
    std::uint64_t loadWorkPumpBudgetSum{};
    int loadWorkPumpBudgetMin{};
    int loadWorkPumpBudgetMax{};
    std::uint64_t loadPumpGateCalls{};
    std::uint64_t loadPumpGateTrueCalls{};
    double loadPumpGateSeconds{};
    double loadPumpGateMaxMs{};
    std::uint64_t loadPumpBusyChecks{};
    std::uint64_t loadPumpBusyTrue{};

    
    
    
    std::uint64_t readGapCount{};
    double readGapAggregateSeconds{};
    double readGapMaxMs{};
    std::uint64_t readGapBucket100us{};
    std::uint64_t readGapBucket1ms{};
    std::uint64_t readGapBucket10ms{};
    std::uint64_t readGapBucket100ms{};
    std::uint64_t readGapBucketLarge{};

    
    
    
    bool waitApiAvailable{};
    std::uint32_t loadingThreadId{};
    std::uint64_t waitCalls{};
    double waitAggregateSeconds{};
    double waitMaxMs{};
    std::uint64_t waitInfiniteCalls{};
    std::uint64_t waitZeroTimeoutCalls{};
    std::uint64_t waitFiniteTimeoutCalls{};
    std::uint64_t waitTimeoutResults{};
    std::uint64_t waitApiCalls[7]{};
    double waitApiSeconds[7]{};
    double waitApiMaxMs[7]{};
    std::uint64_t waitApiTimeoutResults[7]{};

    struct WaitCallsite {
        std::uint64_t callerRva{};
        std::uint64_t calls{};
        double seconds{};
        double maxMs{};
        std::uint32_t apiKind{};
    };
    std::uint32_t waitCallsiteCount{};
    WaitCallsite waitCallsites[12]{};
};

bool LoadingProfilerAvailable();
bool LoadingProfilerSessionActive();
int LoadingStateNow();
std::uint64_t LoadingSessionId();   // (v0.9.521) the last loading session's number (0: none yet)
void BeginOtherLoadProfile();   // a load outside GameStateLoading profiled as its loads (game thread; LoadProf line)
void EndOtherLoadProfile();
bool GetLoadingProfile(LoadingProfile& out);
bool FastLoadingEnabled();
void SetFastLoadingEnabled(bool value);
bool FastLoadingAggressiveRendererAvailable();
bool FastLoadingAggressiveRendererEnabled();
void SetFastLoadingAggressiveRendererEnabled(bool value);
bool FastLoadingNonBlockingPresentEnabled();
void SetFastLoadingNonBlockingPresentEnabled(bool value);


bool FastLoadingRuntimeActive();



bool FastLoadingNonBlockingPresentRuntimeActive();


void RecordLoadingPresent(unsigned int requestedSyncInterval, unsigned int appliedSyncInterval,
                          bool usedDoNotWait, bool wouldBlockDrop, bool otherFailure,
                          std::uint64_t elapsedNanoseconds);
// v0.9.287 frame-time probe: the framework's work in the Present hook and the Present call (microseconds).
void RecordPresentTiming(std::uint64_t frameworkMicroseconds, std::uint64_t presentMicroseconds);
void SyncFromSettings();
bool SkipIntroLogos();
void SetSkipIntroLogos(bool value);
bool DoNotPauseOnFocusLossAvailable();
bool DoNotPauseOnFocusLossEnabled();
void SetDoNotPauseOnFocusLossEnabled(bool value);
bool AllowCampsitesNearEnemiesAvailable();
bool AllowCampsitesNearEnemiesEnabled();
void SetAllowCampsitesNearEnemiesEnabled(bool value);
// (v0.9.586) Enhancements > Gameplay. Hide hints: no tutorial cards / hints (GameTutorialStep::ShowTutorial answers
// "not shown" as for a card already shown enough) and no "basecamp discovered", "tomb discovered", "tomb nearby"
// notifications (Player::QueueNotification drops them). Settings::Get() is updated; the caller saves.
bool HideHintsAvailable();
bool HideHintsEnabled();
void SetHideHintsEnabled(bool value);
// (v0.9.586) Switching weapons (the HUD's weapon select, the number keys) only chooses the weapon while Lara has none in
// her hands: it is made ready on her body, not drawn (with one in her hands the game's swap is kept).
bool NoDrawOnWeaponSwitchAvailable();
bool NoDrawOnWeaponSwitchEnabled();
void SetNoDrawOnWeaponSwitchEnabled(bool value);
// (v0.9.589) With it on, her quiver (what her inventory puts on marker 294) is not drawn while the chosen weapon is not a
// bow: the game's own switch hides it, the chosen-only switch does not (the gear's markers, custom_outfits.cpp).
bool QuiverHiddenForChosenWeapon();
// (v0.9.589) The water surface's height where Lara is (Player +0x1AE8, set by PhysicsCheckSwim) while she is in a water
// volume (Player +0x1AE0); false out of water.
bool PlayerWaterSurfaceZ(float& z);
// (v0.9.586) Lara's draw groups 46-48 (body gear: bandolier, rucksack, satchel) and 12 (the quiver strap) not drawn.
bool PlayerDrawGroupHidingAvailable();
bool HideBodyGearEnabled();
void SetHideBodyGearEnabled(bool value);
bool HideQuiverStrapEnabled();
void SetHideQuiverStrapEnabled(bool value);


bool StartupLegalVisualSuppressionActive();
// Releases faultrep.dll's startup UI hide if it is still active 15 s after first seen (called every Present).
void ServiceStartupUiHideSafety();
// Startup timing summary (v0.9.297): call MarkFrameworkInitFinished at the end of init, ServiceStartupTiming every Present.
void MarkFrameworkInitFinished();
void ServiceStartupTiming();



std::uint64_t StartupTimingMilestoneMs(unsigned int milestone);
void Tick();

float FovMultiplier();
void SetFovMultiplier(float value);

bool PlayerToolsAvailable();
bool TeleportAvailable();
bool InvulnerabilityAvailable();
bool InfiniteHealthAvailable();
bool InfiniteAmmoAvailable();
bool InfiniteResourcesAvailable();




bool WetMaterialTransitionAvailable();
bool WetMaterialTransitionEnabled();
void SetWetMaterialTransitionEnabled(bool value);
float WetMaterialWetInSeconds();
void SetWetMaterialWetInSeconds(float seconds);
float WetMaterialDryOutSeconds();
void SetWetMaterialDryOutSeconds(float seconds);
const char* WetMaterialTransitionStatus();




bool PlayerKillAvailable();
// (v0.9.440) Refused while a cutscene plays (CutscenePlaying): the game has no death inside a cutscene, and the F11
// death camera took the cutscene's camera over.
bool RequestKillPlayer();
// (v0.9.440, the game thread's view, a frame old) A cutscene plays: Kill Lara and outfit switches are refused then.
bool CutscenePlaying();
const char* PlayerKillStatus();
bool DeathTimingAvailable();
float DeathReloadDelay();
void SetDeathReloadDelay(float seconds);




bool MoonJumpAvailable();
bool MoonJump();
void SetMoonJump(bool value);




bool OneHitKillAvailable();
bool OneHitKill();
void SetOneHitKill(bool value);
bool FlyModeAvailable();
bool FlyModeEnabled();
bool SetFlyMode(bool value);
const char* FlyModeStatus();



float DrawInstancesDistance();
void SetDrawInstancesDistance(float distance);


// (v0.9.597) how far from the camera Draw markup draws (it shared Draw collision's distance before)
float DrawMarkupDistance();
void SetDrawMarkupDistance(float distance);




struct UnitInfo {
    std::uint16_t unitId{};
    std::uint32_t runnableId{};
    char releaseName[128]{};
    char sprintName[128]{};
    char storyName[128]{};
    char unitName[128]{};
    char runnableName[256]{};
};

bool UnitControlsAvailable();
void SetUnitSelectorActive(bool active);
const char* UnitStatus();
std::uint64_t UnitListVersion();
int UnitCount();
int CopyUnits(UnitInfo* out, int capacity);
bool RequestUnitLoad(std::uint16_t unitId, const char* unitName, const char* runnableName);
bool RequestUnitLoadByName(const char* unitName);
bool UnitLoadInProgress();





struct OutfitInfo {
    unsigned int outfitIndex{};
    std::uint16_t playerObjectId{};
    int characterProfileIndex{-1};
    char objectName[96]{};
};

bool OutfitControlsAvailable();
// No cutscene outfit changes: the story's scripts (AGPlayerRespawnNode) leave Lara's outfit as it is.
bool NoCutsceneOutfitChangesAvailable();
bool NoCutsceneOutfitChangesEnabled();
void SetNoCutsceneOutfitChangesEnabled(bool value);
void SetCutsceneViewerNoTutorials(bool value);   // while the Cutscene Viewer plays: tutorial cards left out (they pause the game)
void SetCutsceneViewerOutfitLock(bool value);   // as the option while the Cutscene Viewer plays (not saved)
int OutfitCount();
bool GetOutfitInfo(int availableIndex, OutfitInfo& out);
// (v0.9.496) That outfit's dtp::OutfitsData (GlobalCrafting's record: its name, its campsite picture); nullptr: none.
const void* OutfitData(int availableIndex);
std::uint16_t CurrentOutfitObjectId();
bool OutfitSwapInProgress();
// (v0.9.455) The main menu's: the outfit pinned since the last swap let go when the language changed since (its sound
// banks are in the old one).
void ReleaseOutfitPinIfLanguageChanged();
const char* OutfitSwapStatus();
bool RequestOutfitSwap(unsigned int outfitIndex);
bool OutfitDisplayName(int availableIndex, char* out, std::size_t capacity);
void* LocalNetPlayerInfo();
// Hides the drawing of every live instance of these objects (names as in the object list, any case): newly hidden
// count; *found = how many instances of them live.
int HideInstancesOfObjects(const std::vector<std::string>& objectNames, int* found = nullptr);   // found -1: no such object
// The live instances of these objects removed (the game's Instance::EnableRemoveThisFrame): the count; found as above.
int RemoveInstancesOfObjects(const std::vector<std::string>& objectNames, int* found = nullptr);
std::string InstanceObjectName(void* instance);   // its object's name ("?" not known), for the log
int HideInstance(void* instance);   // its drawing hidden (as the cutscenes' hide lists): 1, 0 already hidden, -1 failed
std::string DescribeInstancesNearPlayer(float radius, int maxCount);   // the live instances near Lara (for the log)   // the local player's NetPlayerInfo (null before one exists)
// Game thread: every live instance (full id, the slot in its low 32 bits; object id; position, cm), in slot order; the
// count. Lara's position (cm); an object id's name ("?" not known). For the Cutscene Viewer's look at what moves.
struct InstanceSpot {
    std::uint64_t id{};
    std::uint32_t zone{};
    float at[3]{};
};
int CopyInstanceSpots(std::vector<InstanceSpot>& out);
bool PlayerPosition(float at[3]);
std::string ObjectNameOfZone(std::uint32_t zone);
int LocalPlayerCharacterProfile();
bool SetLocalPlayerCharacterProfile(int profileIndex);

bool RequestNextOutfit();




enum class WeaponClass : int {
    Bow = 0,
    Pistol = 1,
    Smg = 2,
    Shotgun = 3,
};

// WeaponClass of the weapon chosen in the player's inventory, or -1; refreshed every 250 ms on the game thread.
int CurrentWeaponClassCached();
// (v0.9.602) The weapon classes Lara has holstered: bit (1 << WeaponClass) set while an Item of that class has its
// primary instance AWAY on her (InventoryMember state 2: its holster / away marker), read every game tick while
// SetHolsterWatch(true) (custom outfits' toggles shown only while a weapon is holstered); 0 otherwise.
int HolsteredWeaponClasses();
void SetHolsterWatch(bool on);
// (v0.9.602) Game thread: ms of a named part of the tick, summed into the 30 s FrameTime summary's sections (name: a
// string literal; the sections are told apart by its address).
void AddFrameSection(const char* name, double ms);

struct WeaponVariantInfo {
    WeaponClass weaponClass{WeaponClass::Bow};
    bool owned{};
    bool selected{};
    unsigned int inventoryItemId{}; 
    unsigned int objectZoneId{};    
    char displayName[96]{};
    char internalName[96]{};
};






bool DualWieldExperimentalAvailable();
bool DualWieldExperimentalEnabled();
void SetDualWieldExperimentalEnabled(bool enabled);
// One-line Stage 35 state: left holster, re-timed holster/draw alarms, offhand muzzle FX counters.
const char* DualWieldStatus();
// Hosted_DualPistol_Ground_Idle for the lowered (not aiming) idle; off keeps the single-pistol idle.
bool DualWieldDualIdleClipsEnabled();
void SetDualWieldDualIdleClipsEnabled(bool enabled);
// Off: the offhand pistol only follows the right one (no Item registration, no offhand shots).
bool DualWieldNativeOffhandFireEnabled();
void SetDualWieldNativeOffhandFireEnabled(bool enabled);
// Stage 34 behaviour: clear Player::oneHandedCombat every frame (breaks post-combat auto-holster).
bool DualWieldLegacyTwoHandedFlagEnabled();
void SetDualWieldLegacyTwoHandedFlagEnabled(bool enabled);
// (v0.9.462, v0.9.463) Only the pistol that fires plays its fire animation (the other gun's same clip is kept on its
// ready pose).
bool DualWieldFireAnimFilterEnabled();
void SetDualWieldFireAnimFilterEnabled(bool enabled);

bool WeaponControlsAvailable();
void SetWeaponMenuActive(bool active);
const char* WeaponControlsStatus();
std::uint64_t WeaponSnapshotVersion();
unsigned int WeaponVariantCount(WeaponClass weaponClass);
bool GetWeaponVariantInfo(WeaponClass weaponClass, unsigned int index, WeaponVariantInfo& out);
bool RequestEquipWeaponVariant(unsigned int objectZoneId);
bool RequestUnlockWeaponVariant(unsigned int objectZoneId);
bool RequestRemoveWeaponVariant(unsigned int objectZoneId);

// (v0.9.598) Inventory > Equipment: the game's Equipment crafting recipes (dtp::CraftingRecipe of type 5, the campsite's
// Inventory > Equipment grid: quivers, pouches, satchel, rucksack, bandolier, rebreather, ascender, lock pick, ...).
// Owned: the recipe's crafted trait, as the game reads it (v0.9.600: a transient trait on Lara's CapabilitiesComponent,
// any other in her persistent data; the grid's check mark). Giving one does what crafting it does
// (Crafting::LocalPlayerCraftedRecipe without the resources, XP and sound): its item loaded, the trait it replaces
// removed, its crafted trait given (by its kind, as the game's script nodes do), its event sent. Removing one takes its
// crafted trait away and its item when no other owned recipe has that item.
struct EquipmentInfo {
    int recipe{-1};                 // index in GlobalCrafting's recipe list
    int sortOrder{};
    std::uint16_t craftedTrait{0xFFFF};
    std::uint16_t knownTrait{0xFFFF};    // the game's grid lists it once this trait is owned (0xFFFF: always)
    std::uint32_t itemObject{};          // the item crafting loads (0: none)
    bool owned{};                        // its crafted trait owned (as the game reads it: v0.9.600)
    bool known{};                        // in the game's Equipment grid now
    bool itemHeld{};                     // its item in Lara's inventory
    bool transient{};                    // (v0.9.600) its trait is a transient one (Lara's CapabilitiesComponent)
    bool found{};                        // (v0.9.600) found on the way, not crafted (the campsite: "ACQUIRED")
    char traitName[48]{};                // (v0.9.600) the trait table's debug names
    char knownTraitName[48]{};
    char name[96]{};
    char description[384]{};
    char icon[64]{};
    char debugName[64]{};
};
bool EquipmentControlsAvailable();
void SetEquipmentMenuActive(bool active);
const char* EquipmentStatus();
std::uint64_t EquipmentListVersion();
int CopyEquipment(EquipmentInfo* out, int capacity);
bool QueueEquipment(int recipe, bool give);   // recipe -1: every one






enum class CardRarity : int {
    Common = 0,
    Uncommon = 1,
    Rare = 2,
    Heroic = 3,
    Legendary = 4,
};

struct CardInfo {
    unsigned int cardHash{};
    unsigned int cardTypeHash{};
    int sortOrder{};
    float modifier{};
    float xpModifier{};
    CardRarity rarity{CardRarity::Common};
    bool consumable{};
    bool active{};
    char name[128]{};
};

bool CardControlsAvailable();
void SetCardMenuActive(bool active);
const char* CardControlsStatus();
std::uint64_t CardSnapshotVersion();
unsigned int CardCount();
bool GetCardInfo(unsigned int index, CardInfo& out);
bool RequestSetCardEnabled(unsigned int cardHash, bool enabled);





struct SkillInfo {
    std::uint16_t traitIndex{};
    int category{};
    bool enabled{};
    char name[96]{};
};

bool SkillControlsAvailable();
void SetSkillEditorActive(bool active);
const char* SkillStatus();
int SkillPointCount();
std::uint64_t SkillListVersion();
int SkillCount();
int CopySkills(SkillInfo* out, int capacity);
bool RequestGiveSkillPoint();
bool RequestRemoveSkillPoint();
bool QueueSkillEnabled(std::uint16_t traitIndex, bool enabled);



struct ClothStripGroupSettings {
    float gravity{};
    float drag{};
    int maxVelocityUpdateIterations{};
    int maxPositionUpdateIterations{};
    int relaxationIterations{};
    int subStepCount{};
    float windFactor{};
    bool windOnConstraints{};
    float maxMassBounceBackStrength{};
    int transformType{};
    int springStretchinessDefaultPercentage{};
    int springStretchinessLowerPercentage{};
    int rigidityPercentage{};
    float accelerationDivider{};
    float timeDeltaScale{};
    
    
    
    bool collisionEnabled{};
    unsigned int collisionGroupCount{};
    bool collidesExternal{};
    bool collideWithDynamicHair{};
};

struct ClothRuntimeInfo {
    std::uint32_t instanceId{};
    std::uint64_t componentId{};
    std::uint16_t playerObjectId{};
    bool isPlayer{};
    int currentConfigIndex{-1};
    int configCount{};
    int activeStripGroupCount{};
    int totalStripGroupCount{};
};

struct ClothStripGroupInfo {
    int activeIndex{-1};
    unsigned int groupIndex{};
    unsigned int stripCount{};
    unsigned int stripIds[16]{};
    unsigned int shownStripIdCount{};
    ClothStripGroupSettings settings{};
    ClothStripGroupSettings originalSettings{};
    bool hasOriginalSettings{};
};

struct ClothWaterRuntimeInfo {
    bool anyOptionEnabled{};
    bool playerWaterAvailable{};
    bool wetConfigUnderwaterEnabled{};
    bool playerWaterStateFound{};
    bool playerWaterVolumePresent{};
    bool playerInWater{};
    bool wetConfigWanted{};
    bool nativeHoldBreathSignalFresh{};
    bool nativeHoldBreath{};
    unsigned long long nativeHoldBreathSampleAgeMs{};
    float playerWaterZ{};
    int currentConfigIndex{-1};
    int defaultConfigIndex{-1};
    int wetConfigIndex{-1};
    bool wetConfigApplied{};
    bool wetConfigOverriddenByGame{};
};

bool ClothControlsAvailable();
bool ClothLevelCollisionAvailable();
bool ClothLevelCollisionEnabled();
void SetClothLevelCollisionEnabled(bool enabled);
bool ClothLevelCollisionDiagnosticsEnabled();
void SetClothLevelCollisionDiagnosticsEnabled(bool enabled);
bool ClothWaterBehaviorAvailable();
bool ClothWetConfigUnderwaterEnabled();
void SetClothWetConfigUnderwaterEnabled(bool enabled);
bool GetClothWaterRuntimeInfo(ClothWaterRuntimeInfo& out);
void SetClothEditorActive(bool active);
void SetClothEditorTarget(std::uint32_t instanceId, std::uint64_t expectedComponentId);
const char* ClothStatus();
bool GetClothRuntimeInfo(ClothRuntimeInfo& out);
int ClothStripGroupCount();
bool GetClothStripGroupInfo(int listIndex, ClothStripGroupInfo& out);


bool QueueClothStripGroupUpdate(int listIndex, const ClothStripGroupSettings& settings);
bool RestoreClothStripGroupOriginal(int listIndex, ClothStripGroupSettings* restored = nullptr);





struct HairRuntimeInfo {
    std::uint16_t playerObjectId{};
    bool hairEnabled{};
    unsigned int currentSimulationProfile{};
    unsigned int targetSimulationProfile{};
    unsigned int currentRenderingProfile{};
    unsigned int targetRenderingProfile{};
    int simulationProfileCount{};
    int renderingProfileCount{};
    int physicsGroupCount{};
    int renderGroupCount{};
    int constantGroupCount{};
    
    
    
    
    
    unsigned int physicsMappingProfile{0xFFFFFFFFu};
    unsigned int renderMappingProfile{0xFFFFFFFFu};
    float componentRootMotionScale{};
    float underwaterGravity{};
    float underwaterDamping{};
    float targetUpdateRate{};
    float multiStepUpdateRateThreshold{};
    unsigned int resetRelaxSteps{};
    unsigned int lodProfile{};
    unsigned int cinematicLodProfile{};
    unsigned int combatLodProfile{};
    float headRadius{};
    float headFalloff{};
    float headOffset[3]{};
};

struct HairProfileInfo {
    int index{-1};
    int groupCount{};
    bool defaultSetting{};
    char name[96]{};
};

struct HairPhysicsGroupSettings {
    float gravityMagnitude{};
    float damping{};
    float windScale{};
    float rootMotionScale{};
    float rootMotionWindScale{};
    float timeStepScale{};
    bool enableCollision{};
    float restitutionScale{};
    float frictionScale{};
    float velocityWorldShapeMatchingScale[20]{};
    float rootMotionSidewaysPercentage{};
    unsigned int numLocalShapeMatchingIterations{};
    float localShapeMatchingStiffness[8]{};
    float worldShapeMatchingStiffness[8]{};
};

struct HairPhysicsGroupInfo {
    int index{-1};
    unsigned int groupNameHash{};
    unsigned int profileObjectId{};
    HairPhysicsGroupSettings settings{};
    HairPhysicsGroupSettings originalSettings{};
    bool hasOriginalSettings{};
};

struct HairRenderGroupSettings {
    float ambientStrength{};
    float diffuseStrength{};
    float highlightShift{};
    float colorShift{};
    float specStrength1{};
    float specExponent1{};
    float specStrength2{};
    float specExponent2{};
    float colorOverride[3]{};
    float colorOverrideStrength{};
};

struct HairRenderGroupInfo {
    int index{-1};
    unsigned int groupNameHash{};
    unsigned int profileObjectId{};
    float interpMaxDistance{};
    float interpMinMasterDistance{};
    float interpMaxMasterDistance{};
    float interpScaleCurve[8]{};
    HairRenderGroupSettings settings{};
    HairRenderGroupSettings originalSettings{};
    bool hasOriginalSettings{};
};

struct HairStrandColorSettings {
    float rootColor0[3]{};
    float tipColor0[3]{};
    float rootColor1[3]{};
    float tipColor1[3]{};
    float frequency{};
};

constexpr int kHairMaxEditableStrandColors = 8;
struct HairConstantGroupInfo {
    int index{-1};
    unsigned int groupNameHash{};
    float thickness{};
    float shapeCurve[8]{};
    float alphaCurve[8]{};
    float noiseFrequency{};
    float noiseStrength{};
    int totalStrandColorCount{};
    int shownStrandColorCount{};
    HairStrandColorSettings strandColors[kHairMaxEditableStrandColors]{};
};

struct HairRenderingProfileInfo {
    int index{-1};
    int groupCount{};
    bool defaultSetting{};
    unsigned int dirtProfile{};
    float dirtColor[3]{};
    float dirtStrength{};
    char name[96]{};
};

struct HairAdvancedSettings {
    float componentRootMotionScale{};
    float underwaterGravity{};
    float underwaterDamping{};
    float targetUpdateRate{};
    float multiStepUpdateRateThreshold{};
    unsigned int resetRelaxSteps{};
    unsigned int lodProfile{};
    unsigned int cinematicLodProfile{};
    unsigned int combatLodProfile{};
    float headRadius{};
    float headFalloff{};
    float headOffset[3]{};
};

bool HairControlsAvailable();
// (v0.9.586) The hair's motion profile locked by its name (empty: the game chooses): whatever the game switches to, Lara's
// hair keeps that one (Settings::Get().hairLockedMotionProfile updated; the caller saves).
bool HairProfileLockAvailable();
std::string HairLockedMotionProfile();
void SetHairLockedMotionProfile(const char* name);
bool HairLockedProfileMissing();      // the locked name is not one of the worn hair's profiles
int HairLockLastGameRequest();        // the profile the game last asked for while locked (-1: none yet)
void SetHairEditorActive(bool active);
void RequestHairRefresh();
const char* HairStatus();
bool GetHairRuntimeInfo(HairRuntimeInfo& out);
int HairSimulationProfileCount();
bool GetHairSimulationProfileInfo(int index, HairProfileInfo& out);
int HairRenderingProfileCount();
bool GetHairRenderingProfileInfo(int index, HairRenderingProfileInfo& out);
int HairPhysicsGroupCount();
bool GetHairPhysicsGroupInfo(int index, HairPhysicsGroupInfo& out);
int HairRenderGroupCount();
bool GetHairRenderGroupInfo(int index, HairRenderGroupInfo& out);
int HairConstantGroupCount();
bool GetHairConstantGroupInfo(int index, HairConstantGroupInfo& out);
bool QueueHairSimulationProfile(unsigned int profileIndex, float transitionSeconds);
bool QueueHairRenderingProfile(unsigned int profileIndex, float transitionSeconds);
// resetSimulation: the hair snaps back to its rest pose (not needed: the game reads the profiles every frame)
bool QueueHairPhysicsGroupUpdate(int index, const HairPhysicsGroupSettings& settings, bool resetSimulation = true);
bool RestoreHairPhysicsGroupOriginal(int index, HairPhysicsGroupSettings* restored = nullptr);
bool QueueHairRenderGroupUpdate(int index, const HairRenderGroupSettings& settings);
bool RestoreHairRenderGroupOriginal(int index, HairRenderGroupSettings* restored = nullptr);
bool QueueHairRenderingProfileUpdate(int index, const HairRenderingProfileInfo& settings);
// Strand data is baked into the hair when it is created: rebuild = the hair is rebuilt once the edits stop.
bool QueueHairConstantGroupUpdate(int index, const HairConstantGroupInfo& settings, bool rebuild = true);
bool QueueHairAdvancedUpdate(const HairAdvancedSettings& settings, bool resetSimulation = true);
bool QueueHairResetSimulation();
bool QueueHairRebuild();
bool HairRebuildPending();   // a rebuild is queued or waits for the strand edits to stop





struct InstanceInfo {
    std::uint32_t instanceId{};
    std::uintptr_t address{};
    std::uint64_t placementHash{};
    std::uint64_t componentId{};
    std::uint16_t streamUnitId{};
    
    
    bool hasDrawableComponent{};
    bool detailsReady{};
    std::uint64_t noDrawGroupsMask{};
    char name[96]{};
    PlayerTransform transform{};
};

bool InstanceToolsAvailable();
void SetInstanceBrowserActive(bool active);
const char* InstanceToolsStatus();
std::uint64_t InstanceListVersion();
int InstanceCount();
int CopyInstances(InstanceInfo* out, int capacity);
bool RequestInstanceDetails(std::uint32_t instanceId, std::uint64_t expectedComponentId);
bool QueueInstanceTransform(std::uint32_t instanceId, std::uint64_t expectedComponentId,
                            const PlayerTransform& transform);
const char* InstanceTransformStatus();
// The Instances tab's live transform (v0.9.291): the UI calls SetInstanceTransformWatch for the Instance it shows
// every frame (0 to stop; a request older than 0.5 s lapses), the game thread samples it once per tick after that
// tick's queued edits, and GetWatchedInstanceTransform returns the latest sample with the sample counter.
void SetInstanceTransformWatch(std::uint32_t instanceId, std::uint64_t expectedComponentId);
bool GetWatchedInstanceTransform(std::uint32_t instanceId, std::uint64_t expectedComponentId, PlayerTransform& out,
                                 std::uint64_t* sequence = nullptr);
std::uint64_t InstanceTransformWatchSequence();
// Game thread, the Model Viewer (model_viewer.cpp): the Birth Instance path. An object's zone ID by name; its zone
// loaded (a reference the viewer holds until ViewerZoneRelease) and ready; a new Instance of it in a stream unit (at a
// position when given; the instance takes its own reference); an Instance removed at the end of the frame.
bool ViewerObjectsAvailable();
int ViewerPlayerStreamUnit();   // the player instance's stream unit (+0xB0) as the relic viewer births in it; 0: no player
const char* ViewerZoneId(const char* name, std::uint32_t& zoneId);   // nullptr: found (its DRM there); else why not
// (priority: cdc::FileRequest::Priority, 0 INTERRUPT .. 3 NORMAL, the game's own loads' .. 5 LOW)
bool ViewerZoneLoad(std::uint32_t zoneId, int priority);
bool ViewerZoneLoaded(std::uint32_t zoneId);
void ViewerZoneRelease(std::uint32_t zoneId);
void* ViewerBirth(std::uint32_t zoneId, int streamUnit, const float position[3]);
void ViewerRemove(void* instance);
// The viewer's camera: every published camera (the screen's, the camera components') at this place, turned this way
// (cdc quaternion: +Y forward, +X right, +Z up), until SetViewerCamera(nullptr, nullptr). Available with the free
// camera's copy hook.
bool ViewerCameraAvailable();
void SetViewerCamera(const float position[3], const float quaternion[4]);
// Game thread: the viewer's lights, native scene lights in the scene an instance is drawn in (as the light editor's,
// apart from them): made (an index, -1 failed), changed (what differs from the last settings), all released.
struct LightSettings;
bool ViewerLightsAvailable();
void* ViewerSceneOf(void* instance);
// v0.9.427: gameProperties, the light's resource properties as the game builds them (cdc's NativeLightProperties,
// kViewerLightPropertiesSize bytes: the relic viewer's lights), kept as they are but for the settings' range, near
// radius, intensity, colour and shadows (and without the variation, the modulation texture and the animation);
// matrix, 16 floats (rows: the axes, then the position, cdc's) placing the light instead of the settings' position and
// rotation.
constexpr std::size_t kViewerLightPropertiesSize = 0x350;
int ViewerLightCreate(void* scene, const LightSettings& settings, const void* gameProperties = nullptr,
                      const float* matrix = nullptr);
// The settings that describe such properties (false: unreadable).
bool ViewerLightSettingsFrom(const void* gameProperties, LightSettings& out);
bool ViewerLightUpdate(int index, const LightSettings& settings, const float* matrix = nullptr);
void* ViewerLightHandle(int index);   // the light's cdc::ISceneLight* (the scene's SceneLight + 0x2B0); nullptr: none
void ViewerLightsRelease();
// The framework's lights (the light editor's, the viewer's) drawn deferred too, so on the opaque materials as well as
// the forward ones (Scene::TraverseForLights hooked: CommonRenderLight::Draw for each, while enabled); false: forward
// only. Draws: how many so far (any thread).
bool DeferredLightsAvailable();
std::uint64_t DeferredLightDraws();
// v0.9.428: the viewer's lights put in their scene's frame list and made visible for its view before each
// Scene::TraverseForLights (so the forward drawables get them whatever the view's occlusion test says); the counts
// since the last call: already listed by the game, listed by the hook, in no cell (passed by all the same).
bool ViewerLightsKeptVisible();
// v0.9.428: the light a reflection light (as the game's lights of complexity refl: no forward lighting); false: not done.
bool ViewerLightSetReflectionOnly(int index);
// v0.9.429: whether the light's place is in one of its scene's cells now (as its last update left it); the game draws a
// light only then.
bool ViewerLightInCell(int index);
void ViewerLightVisibility(std::uint32_t& listed, std::uint32_t& added, std::uint32_t& noCell);
// Game thread: the Instance* in that slot while it still holds that instance (component id), else nullptr.
void* InstanceNowGameThread(std::uint32_t instanceId, std::uint64_t expectedComponentId);
bool SetInstanceTransformNow(std::uint32_t instanceId, std::uint64_t expectedComponentId, const PlayerTransform& transform);
bool GetInstanceTransformNow(std::uint32_t instanceId, std::uint64_t expectedComponentId, PlayerTransform& out,
                             bool withRotation = true);
// v0.9.423: turned only (Instance::SetRotationEuler, radians; no teleport: its cloth and hair follow); game thread.
bool TurnInstanceNow(std::uint32_t instanceId, std::uint64_t expectedComponentId, const float rotation[3]);
bool SetInstanceNoDrawMaskNow(std::uint32_t instanceId, std::uint64_t expectedComponentId, std::uint64_t mask,
                              std::uint64_t* previous = nullptr);
// The object (zone) id of the instance in that slot now (the editor's Play mode checks a slot still holds the object it
// means before a move or a delete). False when the slot is empty or has no object.
bool InstanceObjectZoneId(std::uint32_t instanceId, std::uint32_t& zoneId);
// Game thread only (the editor's live preview): the live instances within `radius` cm of `center`, nearest first, at
// most `capacity`: slot id, object (zone) id, position (cm) and rotation (cdc::Euler radians). Returns the count.
struct LiveInstance {
    std::uint32_t id{};
    std::uint32_t zoneId{};
    float position[3]{};
    float rotation[3]{};
};
int CopyLiveInstancesNear(const float center[3], float radius, LiveInstance* out, int capacity);
// Game thread (v0.9.436, the Model Viewer's black background before its first model): the Instance* in that slot now
// (whatever it holds), else nullptr.
void* InstanceInSlotGameThread(std::uint32_t instanceId);

bool QueueInstanceDelete(std::uint32_t instanceId, std::uint64_t expectedComponentId);
const char* InstanceDeleteStatus();



bool QueueInstanceDrawGroupMask(std::uint32_t instanceId, std::uint64_t expectedComponentId,
                                std::uint64_t noDrawGroupsMask);
const char* InstanceDrawGroupStatus();




bool InstanceNativeHighlightAvailable();
bool QueueInstanceSelectionHighlight(std::uint32_t instanceId,
                                     std::uint64_t expectedComponentId);
// Green native highlight on the Instance hovered in the Instances list (0, 0 clears). Never shown on the selected one.
bool QueueInstanceHoverHighlight(std::uint32_t instanceId, std::uint64_t expectedComponentId);
// Whether the selected Instance keeps its yellow tint (the hover tint is always on).
void SetInstanceSelectionTintEnabled(bool enabled);
bool InstanceSelectionTintEnabled();







enum MaterialShaderStage : int {
    MaterialShaderStage_Pixel = 0,
    MaterialShaderStage_Vertex = 1,
    MaterialShaderStage_Hull = 2,
    MaterialShaderStage_Domain = 3,
};

struct MaterialInfo {
    std::uintptr_t address{};
    int id{};
    std::uint32_t usageCount{};
    std::uint32_t passCount{};
    std::uint32_t textureCount{};
    std::uint32_t constantCount{};
    std::uint32_t flags{};
    std::uint32_t blendMode{};
    char resourceName[128]{};
    char materialTypeName[96]{};
};

struct MaterialDetails {
    std::uintptr_t address{};
    int id{};
    std::uint32_t usageCount{};
    std::uint32_t passCount{};
    std::uint32_t textureCount{};
    std::uint32_t constantCount{};
    std::uint32_t flags{};
    std::uint32_t blendMode{};
    bool hasWritableConstants{};
    bool hasWritableTextures{};
    char resourceName[128]{};
    char materialTypeName[96]{};
};

struct MaterialTextureInfo {
    int passIndex{-1};
    int stage{MaterialShaderStage_Pixel};
    std::uint32_t entryIndex{};
    std::uint32_t textureSlot{};
    std::uintptr_t textureAddress{};
    std::uint32_t resourceId{};
    bool hasResourceId{};
    float mipLodBias{};
    int width{};
    int height{};
    bool writable{};
    char resourceName[160]{};
};

struct MaterialConstantInfo {
    int passIndex{-1};
    int stage{MaterialShaderStage_Pixel};
    
    
    std::uint32_t index{};
    float values[4]{};
    bool writable{};
};

struct KnownTextureInfo {
    std::uintptr_t address{};
    std::uint32_t resourceId{};
    bool hasResourceId{};
    int width{};
    int height{};
    char resourceName[160]{};
};

struct MaterialScanStats {
    bool active{};
    std::uint32_t renderFrame{};
    std::uint32_t captureEvents{};
    std::uint32_t bindingEvents{};
    std::uint32_t residentRegistrations{};
    std::uint32_t residentSlotsSeen{};
    std::uint32_t captureSlotsSeen{};
    std::uint32_t recentMaterialRefs{};
    std::uint32_t validMaterialRefs{};
    std::uint32_t uniqueMaterials{};
    std::uint32_t registrySlotsProcessed{};
    std::uint32_t registrySlotsTotal{};
    bool registryBuildActive{};
};

bool MaterialEditorAvailable();
void SetMaterialEditorActive(bool active);
const char* MaterialEditorStatus();
std::uint64_t MaterialListVersion();
int MaterialCount();
int CopyMaterials(MaterialInfo* out, int capacity);
bool RequestMaterialRefresh();
bool RequestMaterialDetails(std::uintptr_t materialAddress);
bool GetMaterialDetails(MaterialDetails& out);
std::uint64_t MaterialDetailVersion();
int MaterialTextureCount();
int CopyMaterialTextures(MaterialTextureInfo* out, int capacity);
int MaterialConstantCount();
int CopyMaterialConstants(MaterialConstantInfo* out, int capacity);
int KnownTextureCount();
int CopyKnownTextures(KnownTextureInfo* out, int capacity);
bool GetMaterialScanStats(MaterialScanStats& out);
bool QueueMaterialConstantUpdate(std::uintptr_t materialAddress, int passIndex,
                                 int stage, std::uint32_t index, float value,
                                 bool finalize = false);
bool QueueMaterialTextureUpdate(std::uintptr_t materialAddress, int passIndex,
                                int stage, std::uint32_t entryIndex,
                                std::uintptr_t textureAddress);

// The list's scope: 0 every loaded material, 1 Lara's model's, 2 those of the Instance selected in the Instances window
// (collected on the game thread once a second). MaterialListScope copies the scope's materials (sorted).
void SetMaterialListScope(int scope);
std::uint64_t MaterialListScopeVersion();
std::vector<std::uintptr_t> MaterialListScope();

// What names a material's constants (the Modding Suite's analysis of its pixel shaders, MaterialNames): per pass its
// pixel shader (the shader library's resource ID and its variant 0's DXBC), its pixel textures' slots and its pixel
// constants, read with the details (RequestMaterialDetails).
struct MaterialAnalysisSlot {
    std::uint32_t texture{};      // the texture's resource ID (or an ID made from its address)
    std::uint8_t reg{}, kind{}, cls{};
};
struct MaterialAnalysisPass {
    bool present{};
    std::uint32_t shader{};       // the pixel shader library's resource ID (bit 31: made from its code; 0: none)
    std::vector<MaterialAnalysisSlot> textures;
    std::vector<float> constants; // 4 per register
};
struct MaterialAnalysisShader {
    std::uintptr_t library{};
    std::uint32_t id{};
    std::vector<std::uint8_t> code;   // DXBC
};
struct MaterialAnalysisTextureName {
    std::uint32_t id{};
    std::string name;
};
struct MaterialAnalysisInput {
    std::uintptr_t address{};
    std::uint64_t detailVersion{};    // the MaterialDetailVersion it was read with
    MaterialAnalysisPass passes[10];
    std::vector<MaterialAnalysisShader> shaders;
    std::vector<MaterialAnalysisTextureName> textureNames;
    std::string problem;              // why something could not be read (empty: all read)
};
bool CopyMaterialAnalysisInput(MaterialAnalysisInput& out);




bool MaterialResourceVisibilityAvailable();
// Every live cdc::CommonRenderModelInstance (the constructor/destructor hooks' set), with the tracker's lock held so none
// is destroyed meanwhile (the Modding Suite's live part flags, suite_link.cpp). false: the tracker is not installed.
bool ForEachLiveModelInstance(void (*fn)(void* rmi, void* context), void* context);
// A mesh part hidden in every loaded copy of its model (custom outfits' toggles). The game keeps no model ID with its
// model data, so the copies are found by their layout, as the Modding Suite's live link finds them: FNV-1a 64 of the
// index count, the part and mesh counts and each part's first index and triangle count (ModelAnalysis::ModelLayout).
// part: the part record's number (the instance's prim group).
struct HiddenModelPart {
    std::uint64_t layout = 0;
    std::uint32_t parts = 0;
    std::uint32_t part = 0;
    auto operator<=>(const HiddenModelPart&) const = default;
};
// The parts hidden from now on (the last list replaced; empty: none). Each live model instance of those models gets
// the parts' no-draw bits, as a hidden material's prim groups do (new instances too). false: the model instance
// tracker is not installed (nothing hidden).
bool SetHiddenModelParts(const std::vector<HiddenModelPart>& parts);
// v0.9.451: Lara's gear (the items her inventory puts on her radio, pistol, axe, glowstick, knife, knife sheath, rope
// and belt pouch markers) not drawn while `hidden` (the worn outfit's mod.json "gear": false); drawn as usual in her
// hands. Only the drawing changes (where they are, the radio's voice, the glowstick's light stay). Game thread, every
// tick (the items are looked at again each time).
// v0.9.455: by markers: what her inventory puts on any of `markers` (laracroft's marker IDs; empty: nothing hidden).
// Marker 17 (her climbing axe's) hides the axe on her hip (wp_axe_phys) too. An item of hers on one of them is hidden the
// moment the game asks for it to be drawn (InstanceUtils::SetCapabilities), before the tick has seen it.
bool PlayerGearHidingAvailable();
void SetPlayerGearHiddenGameThread(const std::vector<std::int32_t>& markers);
bool SetMaterialResourceHidden(std::uint32_t resourceId, bool hidden);
// Only this ID (SetMaterialResourceHidden also reaches custom outfits' renamed copies of it).
bool SetMaterialResourceHiddenOne(std::uint32_t resourceId, bool hidden);
bool ToggleMaterialResourceHidden(std::uint32_t resourceId, bool& hiddenNow);
bool IsMaterialResourceHidden(std::uint32_t resourceId);
std::uint64_t MaterialResourceSuppressedDrawCount();


bool RequestMaterialRestore(std::uintptr_t materialAddress);

// .tr10material export (v0.9.307): the material's resource exactly as the game loaded it (reference definitions +
// data, the TrRebootModTools file layout), with this session's constant and texture edits written into it.
// The file is built on the game thread; TakeMaterialSaveResult hands it over once.
bool MaterialSaveAvailable();
bool RequestMaterialSave(std::uintptr_t materialAddress);
struct MaterialSaveResult {
    bool ok{};
    std::uintptr_t material{};
    std::vector<std::uint8_t> bytes;
    std::string fileName;   // "<material name>.<id>.tr10material"
    std::string message;    // what was written, or why nothing could be
};
bool TakeMaterialSaveResult(MaterialSaveResult& out);


void SetMaterialHoverHighlight(std::uintptr_t materialAddress);




struct CapsuleCollisionInfo {
    unsigned int listIndex{};
    std::uint32_t itemId{};
    char name[96]{};
    float matrix[16]{};
    float radius{};
    float length{};
    int segment{};
    float originalMatrix[16]{};
    float originalRadius{};
    float originalLength{};
    int originalSegment{};
};



struct CapsuleCollisionDrawInfo {
    float position1[3]{};
    float position2[3]{};
    float radius{};
};
bool CapsuleCollisionControlsAvailable();
void SetCapsuleCollisionEditorActive(bool active);
void SetCapsuleCollisionTarget(std::uint32_t instanceId, std::uint64_t expectedComponentId);
const char* CapsuleCollisionStatus();
std::uint64_t CapsuleCollisionListVersion();
int CapsuleCollisionCount();
int CopyCapsuleCollisions(CapsuleCollisionInfo* out, int capacity);
bool QueueCapsuleCollisionUpdate(unsigned int listIndex, std::uint32_t expectedItemId,
                                 const float matrix[16], float radius, float length, int segment);
void SetCapsuleCollisionDrawSelection(unsigned int listIndex, std::uint32_t expectedItemId, bool enabled);
void SetCapsuleCollisionInteractiveEditActive(bool active);
std::uint64_t CapsuleCollisionDrawVersion();
int CapsuleCollisionDrawCount();
int CapsuleCollisionDrawKeysScanned();
int CapsuleCollisionDrawKeyMatches();
int CapsuleCollisionDrawRuntimeCandidates();
int CapsuleCollisionDrawRuntimeCapsules();
int CopyCapsuleCollisionDraw(CapsuleCollisionDrawInfo* out, int capacity);




struct CollisionMarkerInfo {
    unsigned int listIndex{};
    std::uint32_t itemId{};
    char name[96]{};
    float matrix[16]{};
    int boneSegment{};
    
    
    
    float originalMatrix[16]{};
    int originalBoneSegment{};
    std::uint16_t boneId{0xFFFFu};
    int boneCount{};
};
bool MarkerControlsAvailable();
void SetMarkerEditorActive(bool active);
void SetMarkerEditorTarget(std::uint32_t instanceId, std::uint64_t expectedComponentId);
const char* MarkerStatus();
std::uint64_t MarkerListVersion();
int MarkerCount();
int CopyMarkers(CollisionMarkerInfo* out, int capacity);
std::uint16_t MarkerBoneIdForSegment(int boneSegment);
bool QueueMarkerUpdate(unsigned int listIndex, std::uint32_t expectedItemId,
                       const float matrix[16], int boneSegment);




bool PlayerMarkerOverrideAvailable();
bool SetPlayerMarkerPositionOverride(const char* markerName, float x, float y, float z);
bool ClearPlayerMarkerPositionOverride(const char* markerName);




struct DrawCameraState {
    bool valid{};
    float position[3]{};
    float quaternion[4]{};
    float fov{};
    
    
    float nearPlane{0.05f};
};

struct DrawInstanceLabel {
    float position[3]{};
    char name[96]{};
    std::uint32_t slot{};         // the Instance ID (its slot)
    std::uint32_t objectId{};     // its object's zone ID (0: unknown)
    std::uint16_t streamUnit{};   // the stream unit it was born in
};

struct DrawTriggerBox {
    float min[3]{};
    float max[3]{};
    std::uint32_t slot{};         // the Instance the TriggerComponent belongs to
    bool enabled{true};           // TriggerComponent +0x48 bit 0x20 (TriggerComponent::Enable)
    char owner[64]{};             // that Instance's name
};

// Markup lines: one MarkUpBox gives one or more lines with the same box index.
struct DrawLine3D {
    float a[3]{};
    float b[3]{};
    std::uint8_t type{};          // the box's markup type (MarkUpBox::GetType; DrawMarkupTypeName)
    std::uint8_t flags{};         // kDrawMarkup* below
    std::uint16_t box{};          // the box's index in the markup pool
};
constexpr std::uint8_t kDrawMarkupDisabled = 1;   // MarkUpBox flags sign bit (MarkUpBox::IsEnabled: no)
constexpr std::uint8_t kDrawMarkupDynamic = 2;    // a dynamic box (moves with its Instance; ropes, ziplines...)
// The game's name of a markup type (MarkupDetectionUtils::MarkupTypeToName); "Unknown" outside 1..25.
const char* DrawMarkupTypeName(int type);

bool DrawToolsAvailable();
bool DrawInstances();
void SetDrawInstances(bool value);
bool DrawTriggers();
void SetDrawTriggers(bool value);
bool DrawMarkup();
void SetDrawMarkup(bool value);
bool DrawMarkupAvailable();   // the markup layout matched this build
bool GetDrawCameraState(DrawCameraState& out);
// Game thread: refreshes the camera GetDrawCameraState returns (the draw tools do it only while one of theirs is on;
// the grapple rope overlay needs it every frame). False when the draw tools are unavailable.
bool SnapshotDrawCameraNow();
std::uint64_t DrawInstanceVersion();
int DrawInstanceCount();
int CopyDrawInstances(DrawInstanceLabel* out, int capacity);
std::uint64_t DrawTriggerVersion();
int DrawTriggerCount();
int CopyDrawTriggers(DrawTriggerBox* out, int capacity);
std::uint64_t DrawMarkupVersion();
int DrawMarkupLineCount();
int CopyDrawMarkupLines(DrawLine3D* out, int capacity);
const char* DrawToolsStatus();
// One line per tool for the Draw menu (empty when that tool is off or has nothing yet).
const char* DrawInstancesStatus();
const char* DrawTriggersStatus();
const char* DrawMarkupStatus();




struct ZoneSuggestion {
    std::uint32_t zoneId{};
    char name[96]{};
};

bool RequestBirthInstance(const char* objectNameOrZoneId);
// The object is born at this position (cm) and rotation (cdc::Euler radians) instead of the player's transform: its
// collision, markers and UI are set up there (an instance moved after its birth keeps its collision where it was
// born). False when unavailable (then RequestBirthInstance + QueueInstanceTransform is the fallback) or busy.
bool BirthAtTransformAvailable();
bool RequestBirthInstanceAt(const char* objectNameOrZoneId, const PlayerTransform& transform);
bool BirthInstanceBusy();
const char* BirthInstanceStatus();
std::uint32_t LastBirthedInstanceId();
int FindZoneSuggestions(const char* queryText, ZoneSuggestion* out, int capacity);




struct LightSettings {
    bool enabled{true};
    int type{0}; 
    float position[3]{};
    float rotation[3]{};
    float color[3]{1.0f, 1.0f, 1.0f};
    float intensity{2.0f};
    
    
    
    float range{800.0f};
    float nearRadius{0.0f};
    float umbra{0.55f};
    float penumbra{0.80f};
    bool indirectLit{true};
    float probePriority{0.0f};
    float indirectIntensity{1.0f};
    float indirectStrength{1.0f};
    float lambertWeight{1.0f};
    float halfLambertWeight{0.0f};
    float ambientWeight{0.0f};
    float specularStrength{1.0f};
    float minShadowSpecular{0.0f};
    float rangeScale{1.0f};
    bool shadowEnabled{false};
    int shadowMapSize{512};
    float shadowMapBias{0.001f};
    bool softShadow{false};
    float softShadowRadius{0.25f};
    int softShadowSamples{8};
    float softShadowDepthBias{0.001f};
    float softShadowExponent{1.0f};
};

struct LightInfo {
    std::uint32_t id{};
    LightSettings settings{};
};




struct GameLightInfo {
    std::uint64_t token{};
    std::uint32_t sceneIndex{};
    std::uintptr_t address{};
    char name[64]{};
    LightSettings settings{};
    LightSettings originalSettings{};
    std::uint32_t lightMask{};
    std::uint32_t shadowMask{};
    bool portalCulling{};
    bool objectCulling{};
    bool hasResourceProperties{};
    bool detailsLoaded{};
    bool modified{};
    int resourceShared{};   // (v0.9.529) the loaded game lights using its resource (its shading values), it among them
    // Read for every light by the roster scan (kept up to date while the editor is open), before its details load:
    float position[3]{};
    bool hasPosition{};
    bool enabled{true};
    bool castsShadows{};
    float color[3]{1.0f, 1.0f, 1.0f};
    float intensity{};
    float range{};
};

bool LightControlsAvailable();
void SetLightEditorActive(bool active);
const char* LightStatus();


std::uint64_t LightListVersion();
int LightCount();
int CopyLights(LightInfo* out, int capacity);
bool RequestCreateLight();
bool QueueLightUpdate(std::uint32_t id, const LightSettings& settings, bool commitResource = false);
bool QueueLightDelete(std::uint32_t id);
bool QueueDeleteAllLights();



std::uint64_t GameLightListVersion();
int GameLightCount();
int CopyGameLights(GameLightInfo* out, int capacity);
bool RequestGameLightRefresh();
bool RequestGameLightDetails(std::uint64_t token);
bool QueueGameLightUpdate(std::uint64_t token, const LightSettings& settings);
bool QueueRestoreGameLight(std::uint64_t token);
bool QueueRestoreAllGameLights();
bool QueueDuplicateGameLight(std::uint64_t token);
// The roster scan: lights read so far of the scene's (both 0 before the first scan); scanning: a pass is running.
void GameLightScanProgress(int& done, int& total, bool& scanning);


bool TimeControlsAvailable();
float TimeScale();
bool SetTimeScale(float value);
bool PauseAvailable();
bool GamePaused();
bool SetGamePaused(bool value);
// (v0.9.491) The game's own pause (its pause queue's paused flag: the pause menu, the map, the inventory...); not the
// framework's F3 pause (GamePaused). Any thread (a read).
bool MenuPaused();

// (v0.9.495) LaraFM (larafm.h): an object in the player's inventory. Game thread.
// 1: she has it (instance: its Instance when one is in the world now, else nullptr), 0: she has not, -1: unknown (no
// player or inventory to read, or no object of that name).
int PlayerInventoryObject(const char* objectName, void** instance);
// An Instance's position now (its +0x10); false when it cannot be read.
bool InstancePositionNow(void* instance, float out[3]);
// The game gives her that object: InventoryComponent::LoadItem(object, 1, -1, false, false), as it gives a missing
// item (it comes once its object has loaded). Game thread. False: no player / inventory, no object of that name, the
// call unavailable on this build, or the game said no.
bool GivePlayerInventoryObject(const char* objectName);



struct WeatherState {
    bool rainActive{};
    float rainIntensity{};
    bool snowActive{};
    float snowIntensity{};
    int timeOfDay{-1};    
    int rawTimeOfDay{-1}; 
    char timeOfDayName[32]{};
};
bool WeatherControlsAvailable();
const char* WeatherStatus();
bool GetWeatherState(WeatherState& out);
// The camera's shakes playing (game thread; -1: no camera): the slots of its shake manager (CameraComponent+0x9E8)
// playing; ownShake: the shake its camera settings started (+0x9F0, its resource ID; 0 none). stop: all of them stopped
// and the own shake forgotten (it starts again with the next camera whose settings ask for one).
int CameraShakesPlaying(bool stop, std::uint32_t* ownShake = nullptr);
bool PrecipitationOverrideEnabled();
bool TimeOfDayOverrideEnabled();
bool SetPrecipitationOverride(bool active, float intensity);
bool SetTimeOfDayOverride(int timeOfDay);
void ClearWeatherOverrides();


bool HudControlsAvailable();
bool HudHidden();
bool SetHudHidden(bool value);




struct GameplayCameraState {
    bool valid{};
    float position[3]{};
    float quaternion[4]{};
    float fov{};
};
bool GameplayCameraAvailable();

bool DepthOfFieldAvailable();
bool DepthOfFieldEnabled();
void SetDepthOfFieldEnabled(bool value);
bool GetGameplayCameraState(GameplayCameraState& out);
void GetGameplayCameraOffsets(float positionOffset[3], float rotationOffsetDegrees[3]);
void SetGameplayCameraOffsets(const float positionOffset[3], const float rotationOffsetDegrees[3]);
void ResetGameplayCameraOffsets();


bool FreeCameraAvailable();
bool FreeCameraEnabled();
const char* FreeCameraStatus();
bool SetFreeCamera(bool value);
float FreeCameraMoveSpeed();
void SetFreeCameraMoveSpeed(float value);
float FreeCameraMouseSensitivity();
void SetFreeCameraMouseSensitivity(float value);
float FreeCameraFovMultiplier();
void SetTrainerMenuOpen(bool open);
void NotifyFreeCameraFocusLost();

bool GetPlayerTransform(PlayerTransform& out);
bool TeleportPlayer(const PlayerTransform& transform);

bool QueuePlayerTransform(const PlayerTransform& transform);

bool Invulnerability();
void SetInvulnerability(bool value);
bool InfiniteHealth();
void SetInfiniteHealth(bool value);
bool InfiniteAmmo();
void SetInfiniteAmmo(bool value);
bool InfiniteResources();
void SetInfiniteResources(bool value);
}
