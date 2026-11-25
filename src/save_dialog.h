#pragma once
#include <cstdint>
#include <string>
#include <vector>

// "Save As" for files the framework builds (v0.9.307: .tr10material export). The dialog runs on its own thread, so
// the game keeps running while it is open; the chosen file is written there too.
namespace SaveDialog {
// False while a dialog is already open.
bool Start(std::vector<std::uint8_t> bytes, const std::string& defaultFileName, const wchar_t* filterName,
           const wchar_t* extension);
bool Busy();
// The last outcome ("Saved to ...", "Save cancelled", or why it failed); `version` changes with each outcome.
std::string Status(std::uint64_t* version = nullptr);
}
