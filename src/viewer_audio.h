#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The Model Viewer's sound (v0.9.431, game thread): a music track of the game's on loop while it is open, the level's
// sounds (the main menu's storm) silenced, and a relic's line on the view (the one the game plays when it is found).
//
// - Ours plays as the game's own sounds do: cdc::SOUND_Play (retail 0x3B8570, a dtp::SoundPlex) with a sound of our
//   own made as the game's relic lines are made (globaluiinfo.drm's DTPs, e.g. 193891: an Assignment, SoundPlex type 6,
//   over a Stream, type 4, the multiplex's name; PlexFactory::CreatePlexHierarchy's jump table, retail 0x39F940), the
//   Assignment's sound group the game's music group (GlobalSoundInfo +0x68 SoundGroupAssignments +0xC "music"; the
//   Assignment's ctor gives the owner its group, retail 0x395ECA), so the music volume and the game's mix apply. The
//   stream is "pcx64-w\audio\streams\<name>.mul". Its owner's root DTP (the sound's m_ownerID, which
//   SoundPlexStream::FillOutCriticalInfo, retail 0x3A2B30, wants loaded) is a relic line's DTP (globaluiinfo.drm is
//   always there). When it ends (a .mul without a loop), it is played again.
// - The level's sounds (v0.9.433; 0.9.431's menu pause groups all paused the music too, the log said: none was used):
//   the ones in the environment's sound groups ("sfx\env...": the storm's rain, wind and thunder) and the menu's music
//   (the music groups' sounds that are not ours), playing when the viewer opens or starting while it is open, faded out
//   through each sound owner's controls' volume (+0x2D8, as the action graphs' sound volume node sets it, retail
//   0x63B0E0) and back to their own volumes after it closes.
// - Ours fades in when the viewer opens, is lowered while a relic's line plays and back after, fades out when it
//   closes and is then ended (its volume the same control's).
// - A relic's line: SOUND_Play (retail 0x3B8540) with its CollectibleItem's m_audioClip (+0x14), as the game's
//   RewardsCollectibleUIMessengerNode plays it: the stream is the voice language's (the game's archives).
// - A character's line (v0.9.443, V): a sound of our own as the music's, its Assignment options and Stream settings a
//   relic line's (copied from the loaded DTP), the stream the line's name.
namespace ViewerAudio {
// clips: the relics' lines (their sound DTPs: the owner of ours); track: the stream's
// name ("" none). The viewer opened.
void Start(const std::vector<std::uint32_t>& clips, const std::string& track);
void Tick();             // game thread, every frame (CutsceneViewer's tick, closed too): the fades; ours again when it ended
void Stop();             // the viewer closed
void PlayVoice(std::uint32_t clip, const char* what);
// (v0.9.443) A voice line by its stream's name (pcx64-w\audio\streams\<name>.mul: the voice language's copy, as the
// game's archives give it), played as a relic's line is (that line's Assignment and Stream settings, its sound group;
// no subtitles): the music lowered under it.
void PlayVoiceStream(const std::string& name, const char* what);
void StopVoice();
bool VoicePlaying();
}  // namespace ViewerAudio
