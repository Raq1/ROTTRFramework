#pragma once
// (v0.9.606) Where a load's time goes, every thread of the game's (ROTTRFramework.ini [Loading] SampleThreads=1; off by
// default: it costs a little during the loads it watches).
//
// The loads' LoadProf lines say how long, how much was read and what the loading thread waited for, not what the other
// threads did (0.9.606's level load: 14.3 s, 30.6 s of CPU, the reads 0.8 s of it). From a load's start
// (Features::BeginLoadingProfileSession: GameStateLoading's loads and the main menu's level) to its end, a thread of its
// own looks every 10 ms at every thread of the process that used CPU over the last 250 ms (and the loading thread; the
// others are left alone: 0.9.606's first level load, all 221 threads each round, cost 23 ms a round), each suspended
// for the few microseconds of copying its registers and
// walking its stack with the modules' unwind data (no allocation, lock or log while it is suspended, as the stall
// sampler does), each sample counted as running or in a system call (a wait, a file read: the instruction before the
// thread's place is a syscall). At the load's end, "LoadSample:" lines: the threads by CPU time over the load (their
// name when the game named them), each with its samples running / in system calls and its most frequent places (the
// game's function by its start, retail RVA; Windows and driver code by module and function start, with the game's
// function that called it: its four most frequent while running, its two while in a system call), then the busiest
// places over every thread, the sampler's own time, and each second's CPU (% of a core) of the six busiest threads
// and the loading thread.
#include <cstdint>

namespace LoadSampler {
// Game thread: a load begins / ends (session: the LoadProf load number).
void Begin(std::uint64_t session);
void End(std::uint64_t session, double seconds);
}  // namespace LoadSampler
