ROTTRFramework - Rise of the Tomb Raider (Steam, DX12/DX11) mod framework: source
==================================================================================

Contents
--------
  src\            framework source (C++20, MASM), one module per feature
  bootstrap\      faultrep.dll proxy that loads the .asi (ROTTR imports faultrep.dll)
  scripts\        data copied next to the .asi (animation clips + ini files, the Cutscene Viewer's frame and
                  pictures: cutscenes\images)
  tools\outfit_manager\
                  material_params.cpp / .h and materials.h: the shader analysis the Material Editor
                  names material parameters with (shared with the Modding Suite, compiled into the .asi)
  tools\re\      shader bytecode helpers (dxbc_disasm.cpp, dxbc_reflect.py)
  tools\verify_retail_offsets.py, tools\memwatch.ps1
                  checks the retail signatures against ROTTR.exe; a memory / VRAM monitor for the game
  third_party\    Dear ImGui, MinHook, Lua 5.4.9, nlohmann/json, stb_image / stb_dxt, bcdec
  docs\          development notes (materials.md: research and every version's changes)
  changes\       each version's diff (framework_<version>.diff; from the next version on)
  CMakeLists.txt  build (faultrep.dll, ROTTRFramework.asi)
  build_vs2026.bat
  build\         made by build_vs2026.bat (the Visual Studio build; deleted and made again each time)
  package\       the build's output: ROTTRFramework.asi, faultrep.dll, ROTTRFramework\ (scripts\ copied)

Main modules (src\)
-------------------
  features.cpp / ui.cpp      core hooks, dual wield and the in-game menu (F8)
  combat_air.cpp             air aim, slow-motion aim, aim camera
  acro.cpp                   acrobatic moves (evade flips, rolls, cartwheels, climb wheel, strafe, jump)
  grapple.cpp                grapple kick
  brawl.cpp                  hand-to-hand combat
  enemy_ai.cpp               enemy close-combat / fire-token behaviour
  swim_breath.cpp            swimming boost and breath handling
  suite_link.cpp, dtp_edit.cpp
                             live link to the ROTTR Modding Suite (named pipe): its DTP and material edits shown
                             in the game at once
  cursor_release.cpp         the game's cursor left free while another window is in front or a Save As dialog
                             is open
  bone_patch.cpp, eight_weight_patch.cpp, dxbc_eight_weight_rewriter.cpp
                             skeleton / skinning limits patches
  blendshape_patch.cpp       blendshape (facial morph) output buffer: 10 MB -> [Rendering] BlendShapeBufferMB
                             (default 64) + overflow guard, fixes corrupted faces on large models in cutscenes
  hair_layout.cpp            PureHair's strand layout put into memory at startup ([Hair] StrandLayout = Stock,
                             PH64, PH128 or PH256; SimTempArray for PH128 / PH256): what rottr_hair.py v12
                             patches into ROTTR.exe, with the exe left as it is (every hair must be exported
                             in that layout, stock hair too; Hair editor > Advanced > Strand layout)
  instance_anim.cpp          Instances window, Animation tab: plays a library clip on one Instance (the game's
                             decompressor writes it into that instance's pose before its bone matrices are built)
  zone_list.cpp              ObjectZone names (Birth Instance, Chapter Select) read from the
                             game's own zonelist.ids table in memory
  save_dialog.cpp            "Save As" dialog on its own thread (Materials window: Save as .tr10material)
  custom_outfits.cpp         custom campsite outfits from ROTTRFramework\outfits\ (*.ini, folders, *.zip
                             packages; mod.json toggles (mesh parts); outfit + character
                             profile entries copied from a base outfit, names, always unlocked, last worn; new
                             object names registered with the game's ObjectZoneManager)
  outfit_archive.cpp         builds bigfile._mod.rottrframework.000.000.tiger (+ .nfo) for custom outfits
                             with new models: the base outfit's .drm with the exported resources swapped in
                             under fresh IDs (tiger/CDRM/DRM reader and writer, DDS -> PCD9)
  zip_reader.cpp             reads .zip mods/packages (stored/deflate) and unpacks them
  game_archive.h             the game's archives (tiger), .drm files and resources (shared with the tools)
  puff.c                     zlib's contrib/puff inflate (reads the game's compressed resources and .zip files)
  glow_color.cpp             Lara's glowstick: in her hand, on her belt or let go (the game's ToggleTorch / DropItem);
                             in a colour of your own and its brightness (contributed; F8 > Glowstick;
                             ROTTRFramework.ini [GlowstickColour]; a lit stick changes at once; "Back to the game's
                             colour" restores the blue / amber)
  language_options.cpp       Options > Audio's voice and text languages enabled in game (the game's own reload)
  steam_save_cache.cpp       the save checks without asking Steam per file; the main menu's missed save data read again

Install
-------
Copy package\ROTTRFramework.asi, package\faultrep.dll and the package\ROTTRFramework\ folder
into the game folder (next to ROTTR.exe). Menu: F8. Log: ROTTRFramework.log, settings:
ROTTRFramework.ini (both in the game folder).

Note: Windows Defender may flag faultrep.dll (the loader proxy) as a false positive.

Build
-----
Visual Studio with "Desktop development with C++" and CMake. Either run build_vs2026.bat (faultrep.dll and
the .asi), or:

  cmake -S . -B build -G "Visual Studio 18 2026" -A x64
  cmake --build build --config Release

The outputs are copied into package\.
