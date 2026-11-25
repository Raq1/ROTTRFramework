#pragma once
// PureHair's strand layout (v0.9.440): what rottr_hair.py (v12) writes into ROTTR.exe, done in memory at startup
// instead ([Hair] StrandLayout in ROTTRFramework.ini). The strand descriptor's and the render vertices' binding word's
// bit fields widened so a strand can have up to 64, 128 or 256 points: the CPU's three descriptor accessors and 71
// immediates in the 11 hair shaders (their checksums made again), before the game makes the hair's shaders (when it
// makes its render device). Every hair the game loads must then be in that layout (exported so, stock hair too).
namespace HairLayout {
enum Layout : int { Stock = 0, PH64, PH128, PH256, Count };
const char* Name(int layout);                  // "Stock", "PH64", "PH128", "PH256"
int Parse(const char* text);                   // -1: none (off: the exe's own layout kept)
// Called once at startup, as early as can be (before the render device is made). wanted: the ini's value; tempArray:
// the simulation shader's per-strand arrays for PH128 / PH256 (0: as many as the layout's points; 16..256).
void ApplyAtStartup(const char* wanted, int tempArray);
int Current();                                 // the layout in memory now (-1: mixed / not read)
const char* Status();                          // what was done this session, in words
}
