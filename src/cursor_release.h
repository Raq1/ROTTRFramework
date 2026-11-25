#pragma once

// The game's cursor left free (v1.0.0, from the Foundation Engine editor's editor_freeze.cpp, v0.9.290, without its
// F10 freeze and the editor's free cursor): while another program's window is in front (the game kept "active" by
// Do not pause on focus loss would otherwise confine the cursor to its window even then), and while one of the
// framework's own windows holds it (the Save As dialog), the game neither hides nor confines the cursor
// (nx::NxGameStateImpl::UpdateCursor, retail 0xC6BB30, is skipped).
namespace CursorRelease {
// Creates the UpdateCursor hook (enabled with the other hooks). Call once at startup, after MH_Initialize.
bool Initialize();
// The framework's own windows (the Save As dialog): while held, the game neither hides nor confines the cursor
// (the game window keeps its input; nothing else changes). Calls pair up: on, then off.
void HoldCursor(bool on);
}
