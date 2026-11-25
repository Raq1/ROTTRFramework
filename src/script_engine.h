#pragma once

namespace ScriptEngine {


bool Initialize();
void Shutdown();



void Tick();
void RequestReload();



void DrawMenuTabs();

int LoadedScriptCount();
const char* Status();
}
