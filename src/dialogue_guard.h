#pragma once
// (v0.9.542) The dialogue manager's dangling reference after Lara is made again (an outfit switch): see
// dialogue_guard.cpp. Always on.
namespace DialogueGuard {
bool Install();   // its hook (before the framework enables them)
}  // namespace DialogueGuard
