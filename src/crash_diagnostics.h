#pragma once

#include <Windows.h>

namespace CrashDiagnostics {



void Install(HMODULE trainerModule);



void SetBreadcrumb(const char* text);
void SetBreadcrumbf(const char* fmt, ...);
}
