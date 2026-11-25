#pragma once
#include <windows.h>

#include <cstdint>
#include <string>

// The Extras menu's MODEL VIEWER (v0.9.410: the game's own 3D view): one of the game's objects born away from the main
// menu's room (above the menu's camera by default; P / L3: in front of it, below it), in its bind pose, and the game's
// camera turned about it, lit by the Modding Suite's studio preset as native scene lights (as the light editor makes
// them).
//
// The object is loaded (ObjectZoneManager::LoadAndAddRef2) and born (Instance::BirthObjectInstance) as the relic
// viewer's FrontEndObjectLoader does, in the player's stream unit (at the main menu: a loaded one), at its place, and
// put back there when it moves (the instance tools' transform write). Its bind pose: written into its pose in the
// AnimBuildTransforms hook (InstanceAnim), its bone matrices rebuilt every frame. Its size: its skeleton's bones in the
// bind pose. The camera: every camera the free camera's copy hook sees published, at the viewer's place.
// v0.9.411: the studio lights drawn deferred too (CommonRenderLight::Draw after their scene's Scene::TraverseForLights:
// they lit only the forward materials before), and a black background (B / R3): nothing of the model's scene drawn
// but the model (Scene::DrawEntity, DrawStaticMeshes, DrawGrass hooked).
// v0.9.412: the studio lights alone (E / Start for the scene's own light): the level's lights (the sun) not drawn
// (LightRenderer::RenderLight), the ambient, second ambient, spherical harmonics and beauty light set to zero after the
// game sets them (ApplyAmbient, ApplySecondAmbient, ApplyIrradiance, UpdateBeautySpecular hooked).
// v0.9.413: a fixed exposure (A / pad A for the game's eye adaptation): BloomRender::SetupTonemapParams hooked, the
// adaptation off while the viewer is open.
// v0.9.416: the screens are the game's own (CutsceneViewer); on the view the model stands alone in the middle and the
// window's input and the pad are the viewer's (turn, move, zoom, lights).
// v0.9.418: the models are the game's own lists (its outfits, weapons, relics, with their names) and two of ours
// (characters, animals); the screens the campsite's Outfits grid and the relic viewer; Lara's outfits (player objects)
// born without the player's components (their hair and cloth kept); no model's sounds unless asked for (S / pad Back on
// the view); the camera's distance from the model's drawn bounds.
// v0.9.419: the types left out are not made either by the makers the game calls from the object's component lists
// (soundskeletal and the gameplay ones: 0.9.418 crashed in SoundSkeletalComponent's init at the first outfit); the relics
// are the collectibles' master collection's (the relic sets' own list is empty in the game's data).
// v0.9.420: a player object is born as any other object: no action graphs (the game births them as instances of their
// own, and Lara's reads the player component: 0.9.419 crashed there), its flags without the player's (0x20000: the code
// that sees it reads the player component), and its AnimComponent kept (the skin, the cloth and the hair follow its
// bones).
// v0.9.421: what a model was born without stays left out until it is gone (its removal's calls too: 0.9.420 crashed
// taking the first outfit away); the model on the left of the grid; PgUp / PgDn on the grid: the category.
// v0.9.422: the grid on the left, the model on the right; the model's size from the Suite's bounds (bounds.json), its
// bones or its drawn bounds; shown once all of it is there; the studio's ambient; the view's button bar clickable.
// v0.9.423: the Suite's look (bounds.json: Lara's outfits as its Gear on Default shows them, their draw groups and hair);
// the Suite's tone mapping (exposure 1.6, white point 4) and reflections (no cube map); the model itself turned (its
// cloth and hair swing), the camera still; the grid's third row (CutsceneViewer).
// v0.9.424: with the Suite's tone mapping none of the game's bloom, light scatter or colour grading; an outfit without
// the Suite's record says so on the screen; the studio lights' scene cells logged.
// v0.9.425: the studio lights light the strand hair (their portal culling on: the dynamic entities find them); no wind
// on the model's hair and cloth (W); the game's ambient occlusion off (O); what the menu asks for logged.
// v0.9.426: the colour correction not drawn at all (with its flags off it raised the frame to the power 2.2: the dark,
// orange look since 0.9.424); the game's ambient occlusion on again by default (O).
// v0.9.427: the relic viewer's lighting instead of the studio's ([ModelViewer] Lighting=Relic; G toggles it): the game's
// relic viewer's three lights (the global object's rig, as CollectibleRender::InitLights converts them) about the camera,
// no ambient, the rig's bloom, no tone mapping, the aperture (1, 0, 0).
// v0.9.428: the forward materials' light no longer comes and goes as the view turns (the lights put in the frame's list
// and made visible before each Scene::TraverseForLights); the rig's refl light a reflection light, as the game's.
// v0.9.429: a light whose place is in none of the scene's cells (which the game does not draw at all: the flicker, the
// black) is moved towards the model along the same line, its range with it, as far as a cell allows.
// v0.9.430: the relics lit by the relic viewer's lighting, every other model by the studio's (no Lighting= setting, no
// G); the skin's subsurface scattering, the game's own (drawn in its cutscenes only), drawn while a model is shown
// ([ModelViewer] SubsurfaceScattering=On by default; U toggles it).
// v0.9.431: the grid's tiles half their size ([ModelViewer] GridScale=0.5), six a row and four rows, at GridLeft /
// GridTop (the movie's 1280 x 720 stage); MODEL VIEWER, the categories' tabs (a click: that category; LB / RB as before,
// no PgUp / PgDn on the grid) and the model's name drawn by the overlay over the grid; the model beside it bigger
// (GridModelX 0.42, GridModelHalf 0.42), turning by itself only for the weapons and the relics; the campsite menu's
// music on loop while open ([ModelViewer] Music), the level's sounds paused, a relic's line on the view (ViewerAudio).
// v0.9.432: the grid's title, tabs and name are the grid movie's own text fields in the game's font (CutsceneViewer),
// not the overlay's.
// v0.9.433: five rows at 0.55 (GridScale, GridTop 112); the lights made before the model is shown, tried every 0.1 s
// and for 10 s a model (they gave up for good after a few models before); the models seen last and the shown one's
// neighbours kept loaded, the shown one loaded at HIGH priority; the music faded in and out, lowered under a relic's
// line, the level's sounds faded out instead of paused (ViewerAudio).
// v0.9.434: Lara's outfits stand as she does in the game ([ModelViewer] OutfitIdle=On; I toggles it): her idle's body
// (ground_idle), eyes (face_eyes on the eye bones) and hair (hair_ground_idle) layered by InstanceAnim::PlayLayers over
// the bind pose; laracroft's object loaded for its animations when they are not.
// v0.9.435: her face too: the mood's face_neutral on its blend shapes and face_eyes' blinks on the eye shapes
// (InstanceAnim writes them into the outfit's blend shape weights); now and then an idle fidget (looking around, a
// neck rub, the necklace) blended over her body (InstanceAnim::AddFidgets).
// v0.9.436: no necklace fidget; the characters stand in the same idle (their own proportions kept); the subsurface
// scattering always on (no setting, no U); H hides the view's UI; the black background from the first frame; KONSTANTIN
// JACKET and REMNANT WARRIOR among the characters; the characters', animals' and relics' names in capitals;
// relic_cm_projector_slides left out; sofia_cine with every draw group; the crowbar relic further from the camera.
// v0.9.437: the characters really stand in Lara's idle (they have no animation component: InstanceAnim builds their
// pose after the game's bind pose); the crowbar framed by the size the game draws it at (about 40 m).
// v0.9.438: the weapons without their upgrades (the base parts most of their upgrades' looks keep), the grid's ALL
// UPGRADES (under the grid on the weapons) with every one; the outfits' search (under the grid on the outfits: a click or
// Ctrl+F, then type); the tiles' check marks (the campsite's outfit worn) hidden; the lights not left close to the next
// relic after the crowbar; the main menu without Leaderboards, the username and the news ticker.
// v0.9.439: (the framework menu's Outfits list: a search field, ui.cpp; nothing here changed)
// v0.9.440: the grid's search on every category (left of it on the weapons: ALL UPGRADES), emptied when the category
// changes; no "_" caret while typing (the box lighter says so) (cutscene_viewer.cpp). Elsewhere: F9 / the Outfits menu
// and F11 / Kill Lara refused while a cutscene plays (features.cpp); PureHair's strand layout in memory
// ([Hair] StrandLayout, hair_layout.cpp).
// v0.9.441: the characters' own idles (kCharacterIdles: a clip of another object's animation library by its animation
// ID, or Konstantin's enter > crossed arms > exit looping as one; that object loaded for it; translations kept as offsets
// from its skeleton, InstanceAnim::ReferenceFromObject; Lara's face over the body as before); the weapons' look without
// upgrades also keeps a part every upgrade either shows or replaces (the axes' handle, group 1: the Reckoner showed its
// head alone).
// v0.9.442: the weapons' look without upgrades as their menu models' scripts start them (kScriptLooks: the base parts of
// each family's action graph; the pistols' barrel and sights, the shotguns' barrel, trigger and ejector were missing);
// Nadia back in Lara's idle.
// v0.9.443: the weapons' looks per menu model as their scripts make them (kMenuLooks: each family's action graph run
// offline as the game runs its graphs, the model's type named value; without upgrades and with every upgrade: ALL
// UPGRADES), parts the script never sets kept; V plays a character's line (kCharacterLines, ViewerAudio::PlayVoiceStream:
// the voice language's copy, the music lowered); W (wind) gone; a character's own idle waits for its source object even
// when the clips are loaded already (its skeleton is the translations' reference: Sofia played a metre up without it).
// In cutscene_viewer.cpp: the grid's name sized to fit its widest word, the search box without the count.
// v0.9.444: the name's words measured in a hidden field of their own (0.9.443 narrowed the name's field to its last
// word); the characters without an animation component posed on every segment, their cloth's too (instance_anim.cpp:
// Sofia's nape hair, on her braids' cloth roots, stayed in the bind pose).
// v0.9.445: that pose built on every segment for real (0.9.444's check of ObjectZone::NumSegments failed on
// bone_patch.cpp's detour: only its tail checked now); the search right of the tabs, in their row, the name's field as
// wide as the grid (less ALL UPGRADES on the weapons, right under the grid); no motion blur while the viewer is open
// (cdc::g_globalMotionBlurEnabled cleared, set again on closing).
// v0.9.446: a relic's line stops again when its view is left (0.9.443's character lines kept it playing).
// v0.9.447: the characters' and the animals' names in the game's text language (ViewerText::Pick; a character's own
// name kept, the rest translated); Jonah Snow's line on V is Jonah London's.
// v0.9.448: the player's bones after its animated ones that no simulated cloth piece moves follow their parents every
// frame (instance_anim.cpp): the Suite's swapped characters' cloth parents (Jacob's coat, Sofia's skirt and braids) stayed
// where the player was born, and the cloth was pulled back there.
// v0.9.449: V on Lara's outfits plays one of ten of her lines at random (kLaraLines), every one of them before any comes
// again; Serafima "The Witch" has a line (610_by_085_390_baba_yaga).
namespace ModelViewer {
enum class Screen { Grid, View };
enum class Kind { Outfits, Characters, Animals, Weapons, Relics };

bool CreateHooks();                        // at startup (CutsceneViewer::CreateHooks): the checks, the hooks
bool Available();                          // that, the Birth Instance functions and the camera hook are there
bool Open();                               // game thread: the lists made, the menu's camera; false: not opened
bool IsOpen();
void Close();                              // game thread: the model removed, the lights released, the camera the game's
void SetScreen(Screen screen);             // game thread
bool OwnsInput();                          // open on the view: the window's input and the pad are the viewer's
bool WantsFrame();                         // open: Draw wanted (the screen's size; the view's mouse)
// (v0.9.508) The scenes left out of the drawing (the main menu's room: black behind the menus), with the hooks the black
// background is made with (Scene::DrawEntity, DrawStaticMeshes, DrawGrass); false: those hooks are not there.
bool SetBackdropHidden(bool hidden);
std::uint32_t TakeBackdropHiddenDraws();   // entities and mesh passes left out so since the last call
// (v0.9.509) The 3D frame dimmed toward black (0: as the game draws it, 1: black) by the tone mapping's last exposure,
// before the menus are drawn over it; while the scenes are left out the eye adaptation is held where it was.
void SetBackdropDarkness(float darkness);
std::uint32_t TakeBackdropDimmed();        // frames dimmed so since the last call
// The lists (game thread, while open)
int CategoryCount();
std::string CategoryName(int category);    // in the game's text language
int ModelCount(int category);
std::string ModelLabel(int category, int index);         // its name (the game's, in its language, or ours)
std::string ModelObject(int category, int index);        // its object
std::string ModelDescription(int category, int index);   // the game's (outfits, weapons, relics), else ""
const void* ModelOutfit(int category, int index);        // the outfit's dtp::OutfitsData (its campsite picture), else null
std::wstring ModelIcon(int category, int index);         // <game>\ROTTRFramework\model_viewer\icons\<object>.png
void Choose(int category, int index);      // the grid's highlight (loaded once it rests), the view's model (at once)
void ChooseCategory(int category);         // the category, at its own place
void Current(int& category, int& index);
unsigned ChoiceSerial();                   // changes when the view's own keys choose another model
bool TakeBack();                           // game thread: the view's Back (Esc, Backspace, pad B) since the last call
bool TakeUiHideToggle();                   // game thread: H on the view since the last call (its UI hidden / shown)
void RequestLine();                        // (any thread, v0.9.443) V on the grid: the highlighted character's line
Kind CategoryKind(int category);
// (v0.9.438) The weapons' upgrades: the game's lists of them read (else the weapons show every part, as before); the
// grid's ALL UPGRADES (the session's, all the weapons): on, a weapon shown with every upgrade it has, else with none.
bool WeaponUpgradesAvailable();
bool AllUpgrades();
void SetAllUpgrades(bool on);
void RequestReset();                       // the view's Reset (its button: the game's legend)
// The grid's place on the movie's 1280 x 720 stage: its scale, its tiles' left edge, its mask's top
float GridScale();
float GridLeft();
float GridTop();
bool InButtonBar(float x, float y, float width, float height);   // the view's button bar (the window's client pixels)
bool ModelShown();
bool ModelLoading();                       // the chosen model not shown yet, nor given up
bool SoundOn();                            // the models' sound components kept (S / pad Back on the view)
std::string Status();                      // what the viewer says about the model: loading, not shown and why ("": shown)
void TickGameThread();                     // game thread, every frame while its screens are the current state
bool HandleKey(UINT msg, WPARAM wParam);   // the window's keys (the view's)
bool FilterPad(unsigned long user, void* xinputState);   // XInputGetState's result: read and emptied on the view
void Draw();                               // inside the overlay's ImGui frame: the screen's size, the view's mouse
// From the AnimBuildTransforms hook (any thread): the viewer's model gets its bind pose.
void OnBuildTransforms(void* instance, std::uint32_t count, std::uint8_t* segs, const std::uint8_t* skeleton);
}  // namespace ModelViewer
