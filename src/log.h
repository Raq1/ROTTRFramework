#pragma once
#include <string_view>

namespace Log {
void Init();
void Write(std::string_view text);
void Writef(const char* fmt, ...);
// Writef, the first time this format is used in the session only: the setup lines a respawn or a level load would
// repeat (what the feature found in Lara's state control, ...). The format string itself is the key.
void WriteOncef(const char* fmt, ...);
// The same three, for the lines only worth reading while working on a feature (what a hook found, each animation,
// each strike, ...): written only with ROTTRFramework.ini [Log] Verbose=1. Without it the log keeps what matters to a
// player: the version, failures, what the Cutscene Viewer and the loads do, hitches. (v1.0.0) Without Verbose, Write and
// Writef also write each kind of line (the format string; a Write's text without its digits) 25 times a session at most,
// then one line saying the rest are left out.
bool Verbose();
void Detail(std::string_view text);
void Detailf(const char* fmt, ...);
void DetailOncef(const char* fmt, ...);
}
