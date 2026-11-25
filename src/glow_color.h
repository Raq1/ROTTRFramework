#pragma once
#include <cstdint>

// Lara's glowstick in a colour of your own (ported from fw v0.9.346/347, re\glowstick\README.txt "COLOUR"). The game's
// glowstick (g_glowstick) has two colours, constants of its light graph: blue RGB (29, 116, 251) and amber (251, 109, 32),
// each a Color_SetColor node (AGMiscSetColorNode). AGMiscSetColorNode::ProcessMsg is hooked: while a colour of your own
// is on, a node that outputs one of the two glowstick constants outputs that colour instead (alpha kept); "the game's
// colour" lets them through again. A lit glowstick changes at once: its intensity (net data 0) is nudged so its graph
// runs the colour again. Brightness: the game's evGlowstickIntensity (0.5 Dim, 1 Normal, 3 Bright) sent to Lara's
// glowstick or flare; while "own brightness" is on it is sent again when a level changes it (net data 3 = the target).
// Kept in ROTTRFramework.ini [GlowstickColour], set from F8 > Glowstick (v0.9.473: its own tab, was F8 > Player).
// v0.9.473: the glowstick in Lara's hand, on her belt, or let go at any time: the game's own
// InventoryComponent::Activate (MakeReady, her hand), Deactivate (PutAway, marker_GlowstickAway on her belt) and DropItem
// on her glowstick item (InventoryType 72) by its object id (v0.9.474; 0.9.473 looked for a "torch" type-20 item). With none, one is given (LoadItem of the stick
// she last let go, or g_glowstick) and taken into her hand when it arrives.
namespace GlowColor {
struct Colour {
    bool custom = false;                     // false = the game's own blue / amber
    std::uint8_t r = 29, g = 116, b = 251;   // the colour of your own
    bool ownBrightness = false;              // false = the game's (its levels dim / brighten it)
    float brightness = 1.0f;                 // the game's scale: 0.5 Dim, 1 Normal, 3 Bright (0.1..5)
};
constexpr float kBrightnessMin = 0.1f, kBrightnessMax = 5.0f;

// Before MH_ApplyQueued: creates the hook (enabled with MH_ALL_HOOKS).
bool Initialize();
bool Available();
const char* Status();

Colour Current();
void Set(const Colour& colour, bool save);

// Game thread, every tick: a lit glowstick takes a new choice; the hand / belt / let go requests are carried out.
void TickGameThread();

enum class HoldRequest { Hand = 1, Belt = 2, LetGo = 3 };
bool HoldAvailable();
void RequestHold(HoldRequest request);   // any thread; done on the game thread's next tick

void DrawTab();   // F8 > Glowstick

// (v0.9.477) While a stick is kept on her belt, Lara's state control is not let into its glowstick-equip states
// (default_to_glow_stick_equip / _spawned: the reach for the stick on her hip): CombatAir's CheckConditions hook asks.
bool HoldBackTransition(const char* transitionName);
}
