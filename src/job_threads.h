#pragma once
// (v0.9.606) More threads for the game's background jobs: its Deflate decoding during loads.
//
// The game's job system (retail 0x1F0320, from 0x1F02B0 with the process's processor count, called once by the
// game's start at 0x1A1FD6) makes three groups of worker threads (start 0x1F07B0, a semaphore each): min(n, 16) above
// normal, min(n, 16) normal, and min(n, 2) below normal; their total at [0x165B898] (at most 34: the threads' index
// table at 0x165B7E0 holds 34). The third group runs the loads' Deflate jobs (0x342100: the z_stream and its state on
// the job's own stack; 0x34E3F0, zlib's inflate_fast, half of every thread's running samples in a level load, on those
// two threads, each 70-75% busy all along: load_sampler.h). Here, before the game starts (DllMain: the job system is
// made later, from WinMain), the third group's cap is raised: min(n, 2) becomes min(n, K) with K half the processors
// (at least 2) and the total kept within the table's 34 (a CPU with 16 or more threads has no room: unchanged). The
// bytes: 0x1F0357 mov r8d, 2 (the cap; r8d + 1 was also the semaphores' count, 3) -> mov r8d, K; 0x1F0374
// lea ebp, [r8 + 1] -> push 3 / pop rbp / nop (3 semaphores as before). Each instruction around them is checked first.
// ROTTRFramework.ini [Loading] BackgroundThreads: 0 the game's (2), else that many (kept within the table); without
// the key, K with [Loading] FasterLoading on, the game's with it off.
#include <cstdint>

namespace JobThreads {
// DllMain (the loader's lock held: no logging, no loading): the patch, if wanted and the job system is not made yet.
void PatchEarly();
// Later (the log open; StartupTiming's: the job system is made after the framework's init ends): what PatchEarly
// did, and the groups the game made.
void LogResult();
}  // namespace JobThreads
