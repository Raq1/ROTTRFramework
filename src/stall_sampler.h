#pragma once
// Where the game stops presenting frames (v0.9.339).
//
// The main menu's first second and the click on Continue / Load Game stall the game for hundreds of milliseconds to
// seconds, with the framework's own ticks short (the v0.9.338 log: 4.4 s without a frame after the menu appeared, the
// framework's longest tick 131 ms). A watchdog thread (started by the first Present) notices Present stopping (250 ms
// without a frame) and, until frames come back, samples the call stacks of the game's tick thread and its Present
// thread every 25 ms: the thread is suspended for the few microseconds of copying its registers and walking its stack
// with the modules' unwind data (no allocation, lock or log while it is suspended). Each stall then gets one "Stall:"
// line: when, how long, and the most frequent stacks as module+offset (the framework's own frames by name when its PDB
// is next to it). At most 30 a session, none for a stall during which the game was not the foreground window.
// v0.9.470: a stall still on after 10 s with the game in front all along (a hang, as the black screen at startup) is
// reported then, and after 15 s a minidump of every thread is written next to the game (ROTTRFramework_hang.dmp,
// once a session).
// v0.9.592: that report also has every thread's stack once (grouped) and the render device's frame state, and the
// dump is written once such a hang was reported, even if the game is no longer in front. And the hang it caught (the
// render flush waiting in RenderPassData::ExecutePasses for a pass event no job will set, the game thread waiting
// behind it) is put right: that wait lasting 3 s while no frame comes, the event is set and logged.
namespace StallSampler {
// The game's tick (Gameloop::DoTimeProcess) and Present, every frame.
void GameTickBeat();
void PresentBeat();
}  // namespace StallSampler
