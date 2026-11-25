#pragma once
#include <cstdint>
#include <string>

// Animation tab of the Instances window: plays one animation-library clip on a single Instance.
//
// The game's AnimFragmentNodes (the clip slots of an animation graph) are shared by every instance of the same
// object, so a clip is not put into the graph. Instead the clip is decoded on its own, with the game's decompressor
// (cdc::DecompressFrame::DecompressFrameWithBoneSet), into the instance's final pose right before
// cdc::AnimBuildTransforms turns that pose into bone matrices. The channels the clip has replace the graph's;
// bones the clip does not animate keep the graph's pose. Only the chosen instance is affected.
namespace InstanceAnim {
// Checks the retail layouts and creates the AnimBuildTransforms hook unless CombatAir already did (then its hook
// calls OnBuildTransforms). Enabled by the init's MH_ALL_HOOKS.
bool CreateHooks();
bool Available();
const char* UnavailableReason();

// From the AnimBuildTransforms hook (any thread): the pose of an instance that plays a clip gets that clip's frame.
void OnBuildTransforms(void* instance, std::uint32_t count, std::uint8_t* segs, const std::uint8_t* skeleton);
// The blend shape hook (AnimComponentV2::UpdateRenderModelBlendShapes) is created by CreateHooks too.
// Game thread: the instance's bone matrices rebuilt every frame (TransformComponent::EnableForceRebuildTransforms), so
// AnimBuildTransforms runs for it wherever it is (the Model Viewer's model).
void KeepPoseBuilt(void* instance);
// Once per game tick (Gameloop::DoTimeProcess hook): list builds, play requests, dropping instances that went away;
// (v0.9.448) which instance is the player's (its bones after the animated ones that no simulated cloth piece moves are
// posed from their parents every frame: see PoseLooseSegments).
void TickGameThread();

struct AnimListEntry {
    std::uint32_t libraryKey{};
    std::uint16_t index{};      // entry in the library
    std::int16_t animId{};      // the animation's id (shown as "name (id)")
    std::uint16_t keys{};       // frame count
    std::int16_t timePerKey{};  // game time units per frame (100 = 1/30 s)
    char name[80]{};
    char library[48]{};         // last part of the library's path
};

// The list the search field offers for an Instance: its own animation libraries (the graphs it runs), or every
// loaded library. Built on the game thread; ListVersion() changes when a new list is ready.
void RequestList(std::uint32_t instanceId, std::uint64_t componentId, bool allLibraries);
std::uint64_t ListVersion();
// The list's instance and size; false while no list was built for it yet.
bool GetListInfo(std::uint32_t instanceId, std::uint64_t componentId, int& entries, int& libraries, bool& allLibraries);
const char* ListStatus();
// Fuzzy match on the name, or the id when the query is a number. Best matches first.
int FindAnimations(const char* query, AnimListEntry* out, int capacity);

bool RequestPlay(std::uint32_t instanceId, std::uint64_t componentId, const AnimListEntry& entry, bool loop, float speed);
void Stop(std::uint32_t instanceId, std::uint64_t componentId);
void StopAll();

struct PlaybackInfo {
    bool playing{};
    bool paused{};
    bool loop{};
    float speed{1.0f};
    float time{};       // seconds
    float duration{};   // seconds
    int frame{};
    int frames{};
    std::int16_t animId{};
    char name[80]{};
};
bool GetPlayback(std::uint32_t instanceId, std::uint64_t componentId, PlaybackInfo& out);
void SetLoop(std::uint32_t instanceId, std::uint64_t componentId, bool loop);
void SetSpeed(std::uint32_t instanceId, std::uint64_t componentId, float speed);
void SetPaused(std::uint32_t instanceId, std::uint64_t componentId, bool paused);
// Jumps to a frame (pauses playback there).
void SeekFrame(std::uint32_t instanceId, std::uint64_t componentId, float frame);
const char* PlayStatus();

// Layers (v0.9.434, the Model Viewer's idle for Lara's outfits): clips played at once on one instance, each over the
// ones before it and only on its bones (its tracks of those global bone IDs; the others left out of its id map), each
// looping on its own clock at its own speed; the first layer is the player's own clip (the Animation tab shows it).
// v0.9.435: a layer can also set blend shapes (the face): its tracks of those global blend shape IDs, decoded with the
// clip and written into the instance's AnimComponentV2 weights right before the game hands them to the render model
// (AnimComponentV2::UpdateRenderModelBlendShapes, hooked). The other clips here set no blend shapes.
constexpr int kAllTracks = -1;   // (boneCount / shapeCount) every track of that kind the clip has
struct Layer {
    const char* name = nullptr;              // the clip's name in its library (e.g. "ground_idle")
    float speed = 1.0f;
    const std::uint16_t* bones = nullptr;    // the global bone IDs it sets
    int boneCount = kAllTracks;              // kAllTracks: all its bone tracks; 0: none
    const std::uint16_t* shapes = nullptr;   // the global blend shape IDs it sets
    int shapeCount = 0;                      // kAllTracks: all its blend shape tracks; 0: none
    // (v0.9.441) The clip by its animation ID instead of its name (name null): the one a loaded library lists with this
    // ID (the Suite's "Animation <ID>"), in any library. The first layer can be a sequence instead: up to three clips by
    // ID played back to back (an enter, a loop and an exit), the whole looping.
    std::int16_t animId = 0;
    const std::int16_t* sequence = nullptr;
    int sequenceCount = 0;
    // (v0.9.475) Only the instance's skeleton segments whose mask byte is set (mask[segment index] != 0; segments past
    // segmentMaskCount left out); null: every segment its tracks reach (CombatAir: Lara's legs on a beam while aiming).
    const std::uint8_t* segmentMask = nullptr;
    int segmentMaskCount = 0;
    // (v0.9.481, CombatAir's upper body on a beam) Weights: the layer blended into the pose below it at this weight
    // (1: written over it, as before; 0: left out), changed later with SetLayerWeights. sumWithFirst (a layer after the
    // first): blended together with the first layer, as one weighted sum over the pose below (the first layer's weight
    // and these add up to how much of the pose they take; e.g. the aim's level / high / low by the camera's pitch).
    // once: played once and held on its last frame (the first layer: the player's clip), from PlayLayers or the last
    // RestartLayer, on its own clock.
    float weight = 1.0f;
    bool sumWithFirst = false;
    bool once = false;
};
// Game thread. The clips are looked for in the loaded animation libraries: the one that has the most of them loaded
// (all of them, or false). info: what was found or why not (the library, each clip's frames and tracks).
// reference (v0.9.436): the bind translations of the skeleton the clips were made on, for an instance of another one
// (another character): a translation a clip sets on one of these bones is taken as an offset from it, onto the
// instance's own bind translation (its proportions kept); the rotations are the clip's as they are (cdc bind poses
// have none). The fidgets AddFidgets adds get it too. None: the clips' translations as they are.
struct BindPivot {
    std::uint16_t bone = 0;   // global bone ID
    float pivot[3]{};         // its bind translation from its parent (cm)
};
bool PlayLayers(std::uint32_t instanceId, std::uint64_t componentId, const Layer* layers, int count, std::string& info,
                const BindPivot* reference = nullptr, int referenceCount = 0);
// (v0.9.481) The layers' weights now (weights[i] for layer i, as PlayLayers had them; any thread). False: no player.
bool SetLayerWeights(std::uint32_t instanceId, std::uint64_t componentId, const float* weights, int count);
// (v0.9.481) A layer's clock back to its start (layer 0: the player's own clip). False: no player or no such layer.
bool RestartLayer(std::uint32_t instanceId, std::uint64_t componentId, int layer);
// (v0.9.441) Whether a loaded library lists a clip with this animation ID (game thread).
bool ClipLoaded(std::int16_t animId);
// (v0.9.441) A reference for PlayLayers from another object's skeleton: the bones these clips (by animation ID, loaded)
// set translations on, with that object's bind translations. objectData: the object's shared zone data (what its
// instances' ObjectZone +8 is: the zone map's holder data). Returns how many (0: none of those bones, or the object has
// no skeleton), -1 on a fault or a clip not loaded. Game thread.
int ReferenceFromObject(const void* objectData, const std::int16_t* animIds, int count, BindPivot* out, int capacity);

// Fidgets (v0.9.435): once in a while (a random wait of minGap..maxGap seconds after the last), one of these one-shot
// clip sequences (up to 3 clips played back to back, e.g. an enter, a loop and an exit) is played over the player's own
// clip on all its bones, faded in and out over fade seconds (blended in the pose: the game's quaternion-log blend, its
// logs wrapped to the pose's). The layers after the first stay over it. A fidget whose clips are not all loaded is
// left out. Game thread, after PlayLayers; false when none of them is there (info: what was found).
struct Fidget {
    const char* parts[3] = {};
    int partCount = 0;
};
bool AddFidgets(std::uint32_t instanceId, std::uint64_t componentId, const Fidget* fidgets, int count, float minGap,
                float maxGap, float fade, std::string& info);
}  // namespace InstanceAnim
