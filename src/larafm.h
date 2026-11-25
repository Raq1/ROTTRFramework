#pragma once

// (v0.9.491) LaraFM: your own music on a radio in the game. The files in ROTTRFramework\LaraFM (and its subfolders:
// mp3, m4a, mp4, aac, wav, flac, wma, ogg) play through the game's own FMOD Ex (4.44.46, statically linked in
// ROTTR.exe), on a channel of its own with a radio filter, from Lara's own radio only (v0.9.493; 0.9.491 / 492 also
// played from radio props in the levels): g_walkie_talkie, the item on her marker_PDA that the game's radio voices
// come from, placed by its own Instance (v0.9.495: the item of her inventory; never from Lara herself). Without it in
// her inventory nothing plays ("No radio - can't listen to music!", and F8 > LaraFM gives it). A load of the game's
// stops it (v0.9.494); silent while there is no Lara (the main menu) and while the game is paused. Arrow keys in play:
// Up plays (a random track; a paused one resumes), Down pauses / resumes, Left the previous track, Right the next.
// F8 > LaraFM: the track list, Play, Pause, Stop, Previous, Next, Volume, Shuffle and the radio effect's strength.
//
// - FMOD: the C++ API's wrappers the game links (retail; no symbols there, matched to TR2_x64_final's by their code,
//   one block 0x4A550 lower, the DSP ones 0x4A600; every one byte-checked): System::update 0xA5ABA0 (hooked: every
//   LaraFM call to FMOD is made there, on the thread the game updates FMOD on), createSound 0xA5A2C0, playSound
//   0xA5A890, createDSP 0xA5A240, Channel::addDSP 0xA5AFF0, getPosition 0xA5B0F0, setPaused 0xA5B2E0, setSpeakerMix
//   0xA5B350 (this build: one array of 8 levels), setVolume 0xA5B3B0, stop 0xA5B3F0, Sound::getLength 0xA5ACD0,
//   release 0xA5AE00, setDefaults 0xA5AEA0, DSP::release 0xA5BB70, remove 0xA5BBA0. FMOD_CREATESOUNDEXINFO is 4.44's
//   (cbsize must be 0xD8: createSoundInternal checks it; pcmreadcallback +0x38 and pcmsetposcallback +0x40 copied into
//   the sound, numchannels +0x0C, defaultfrequency +0x10, format +0x14 read by CodecUser), FMOD_DSP_DESCRIPTION too
//   (SystemI::createDSP copies +0x20 .. +0x80; DSPFilter::read calls read(state, in, out, length, inch, outch)).
//   The game positions its 3D sounds itself (Channel::set3DAttributes is not linked): LaraFM does the same, a 2D
//   software channel with volume and speaker levels from the source's place seen from the camera.
// - Decoding: Windows Media Foundation (a source reader, 16-bit PCM, to stereo) on a thread of its own into a ring
//   buffer that FMOD's user stream (FMOD_OPENUSER | FMOD_CREATESTREAM) reads; what Windows cannot open (ogg: no
//   Media Foundation handler for it) goes to FMOD's own decoders from memory (FMOD_OPENMEMORY_POINT).
// - The radio sound: a DSP of LaraFM's own on the channel (mono, band-passed 320 .. 3400 Hz, a mid lift, a soft
//   clip, a little hiss), mixed with the music by "Radio effect".
// - The game's pause (its pause queue: the pause menu, the map...) pauses it too.
// - (v0.9.586) Under water ("Muffled under water", [LaraFM] UnderwaterMuffle, default on): while Lara's head is under
//   (SwimBreath::LaraUnderwater) the DSP blends in two more low-passes per channel (420 Hz) over 0.15 s, a little quieter.
//   (v0.9.589) Under water is her radio's height below the water's surface (Features::PlayerWaterSurfaceZ); 550 Hz; its
//   level "Under water volume" ([LaraFM] UnderwaterVolume, default 1).
// - (v0.9.590) "Play during cutscenes" ([LaraFM] PlayInCutscenes, default on): off, held silent while a cutscene plays
//   (Features::CutscenePlaying), on again where it was after.
namespace LaraFM {
bool Install();          // before MH_ApplyQueued: the System::update hook (enabled with MH_ALL_HOOKS)
bool Available();
void TickGameThread();   // her radio (in her inventory, its place), Lara there or not, the game's pause; giving it
// The game window's keys (its window procedure): the arrow keys while in play. Never swallows them.
void HandleKey(unsigned int msg, unsigned long long wParam, long long lParam);
void DrawTab();          // F8 > LaraFM
}  // namespace LaraFM
