#pragma once
// Mods' cutscene subtitles worded otherwise (mod.json "subtitles": the Modding Suite's subtitle editor).
//
// A cutscene's subtitles are the frame records of a CINE stream (its sound stream's, in the language archives: each
// voice language has its own copy, timed to its own voices; the cutscene's .mul can have some too). A record holds
// every text language's block, "<language>\r<block>\r<language>\r<block>...": the block is the lines shown at that
// frame, split at '\n', each "[tags]text" (Subtitle::ProcessSubtitle reads the tags). The game shows the subtitle
// language's block: cdc::CinematicHandlerImpl::ProcessSubtitle (TR2 0x14017e140; retail RVA 0x16a830, the same code)
// copies it into a 1024-byte buffer, splits it with strtok and hands each line to Subtitle::Add.
//
// The hook hands ProcessSubtitle the record with the mods' lines in place of the game's: a line (a block's piece
// between '\n's, "[tags]text" as stored) is replaced where its language and its text are the ones an edit names, in
// whichever voice language's copy and however it is timed or grouped there. The mod's text can be several lines ('\n'
// between them) or none (empty: strtok leaves the piece out, the line is not shown). An edit is for one cutscene: the
// playing one is the handler's (CinematicHandlerImpl +0x38, as CinematicHandler::GetDebugCineName returns it: the
// name of the multiplex stream it plays, cinematic\<chapter>\<name>); without a cutscene name, for any. When several
// mods name the same line, the first mod read wins.
#include <string>
#include <vector>

namespace SubtitleEdit {
struct Edit {
    int language = -1;        // the text language (language_t: 0 English, 1 French...)
    std::string original;     // the line as the game has it (UTF-8, its tags included)
    std::string text;         // the mod's (lines split by '\n'; empty: none)
    std::string cinematic;    // the cutscene's multiplex name (lower case; empty: any cutscene)
    std::string mod;          // (for the log)
};
// Before Install.
void Add(const std::vector<Edit>& edits);
// The hook (when a mod has edits). false: none made.
bool Install();
}  // namespace SubtitleEdit
