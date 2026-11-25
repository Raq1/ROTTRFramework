cmake_minimum_required(VERSION 3.25)
project(ROTTRFramework LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)




if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()



if(MSVC)
  enable_language(ASM_MASM)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")



  string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

include(FetchContent)
# The libraries are in third_party\ (Dear ImGui, MinHook, Lua: their sources at the versions below), so the build needs
# no network. Without a copy, each is downloaded as before (a stalled download gives up instead of hanging).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui.cpp")
  set(imgui_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
else()
  FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG 45acd5e0e82f4c954432533ae9985ff0e1aad6d5
    TIMEOUT 120
  )
  FetchContent_GetProperties(imgui)
  if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
  endif()
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/src/hook.c")
  set(MINHOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook")
  add_library(minhook STATIC
    ${MINHOOK_DIR}/src/buffer.c
    ${MINHOOK_DIR}/src/hook.c
    ${MINHOOK_DIR}/src/trampoline.c
    ${MINHOOK_DIR}/src/hde/hde64.c
  )
  target_include_directories(minhook PUBLIC ${MINHOOK_DIR}/include)
  target_include_directories(minhook PRIVATE ${MINHOOK_DIR}/src ${MINHOOK_DIR}/src/hde)
else()
  FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4
    GIT_SHALLOW TRUE
    TIMEOUT 120
  )
  FetchContent_MakeAvailable(minhook)
endif()

# Lua 5.4.9: the copy in third_party\lua (its sources, from github.com/lua/lua tag v5.4.9); without it, downloaded from
# lua.org (a stalled download gives up after a minute instead of hanging the build).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua/src/lapi.c")
  set(lua_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua")
else()
  FetchContent_Declare(
    lua
    URL https://www.lua.org/ftp/lua-5.4.9.tar.gz
        https://www.tecgraf.puc-rio.br/lua/ftp/lua-5.4.9.tar.gz
    URL_HASH SHA256=2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    TIMEOUT 60
    INACTIVITY_TIMEOUT 20
  )
  FetchContent_GetProperties(lua)
  if(NOT lua_POPULATED)
    FetchContent_Populate(lua)
  endif()
endif()
file(GLOB LUA54_SOURCES CONFIGURE_DEPENDS "${lua_SOURCE_DIR}/src/*.c")
list(REMOVE_ITEM LUA54_SOURCES
  "${lua_SOURCE_DIR}/src/lua.c"
  "${lua_SOURCE_DIR}/src/luac.c"
  "${lua_SOURCE_DIR}/src/onelua.c"
)
add_library(lua54 STATIC ${LUA54_SOURCES})
target_include_directories(lua54 PUBLIC ${lua_SOURCE_DIR}/src)
target_compile_definitions(lua54 PRIVATE LUA_USE_WINDOWS)
if(MSVC)
  target_compile_options(lua54 PRIVATE /W0)
endif()






add_library(faultrep SHARED bootstrap/faultrep_proxy.cpp)
set_target_properties(faultrep PROPERTIES OUTPUT_NAME "faultrep" PREFIX "" SUFFIX ".dll")
if(MSVC)
  target_compile_options(faultrep PRIVATE /W4 /GS- /GR- /EHs-c- /Zl /Oi-)
  target_link_options(faultrep PRIVATE /NODEFAULTLIB /ENTRY:DllMain /SUBSYSTEM:WINDOWS)
  target_link_libraries(faultrep PRIVATE kernel32)
endif()
add_custom_command(TARGET faultrep POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:faultrep> ${CMAKE_SOURCE_DIR}/package/faultrep.dll)

add_library(rottr_framework SHARED
  src/dllmain.cpp
  src/log.cpp
  src/memory_probe.cpp
  src/stall_sampler.cpp
  src/load_sampler.cpp
  src/job_threads.cpp
  src/steam_save_cache.cpp
  src/cutscenes.cpp
  src/cutscene_viewer.cpp
  src/image_load.cpp
  src/viewer_text.cpp
  src/cutscene_titles.cpp
  src/crash_diagnostics.cpp
  src/game_build.cpp
  src/zone_list.cpp
  src/save_dialog.cpp
  src/custom_outfits.cpp
  src/outfit_menu_rows.cpp
  src/outfit_archive.cpp
  src/sound_replace.cpp
  src/subtitle_edit.cpp
  src/voice_edit.cpp
  src/weapon_mods.cpp
  src/silencer.cpp
  src/hud_watch.cpp
  src/dialogue_guard.cpp
  src/rt_probe.cpp
  src/rt_probe_gpu.cpp
  src/rt_cutout.cpp
  src/dx12_root_fix.cpp
  src/dlaa.cpp
  src/render_tweaks.cpp
  src/larafm.cpp
  src/zip_reader.cpp
  src/puff.c
  src/bone_patch.cpp
  src/eight_weight_patch.cpp
  src/shadow_blend.cpp
  src/blendshape_patch.cpp
  src/hair_layout.cpp
  src/dxbc_eight_weight_rewriter.cpp
  src/settings.cpp
  src/script_engine.cpp
  src/features.cpp
  src/swim_breath.cpp
  src/combat_air.cpp
  src/instance_anim.cpp
  src/model_viewer.cpp
  src/viewer_audio.cpp
  src/acro.cpp
  src/grapple.cpp
  src/enemy_ai.cpp
  src/brawl.cpp
  src/glow_color.cpp
  src/language_options.cpp
  src/suite_link.cpp
  src/cursor_release.cpp
  src/dtp_edit.cpp
  src/overlay.cpp
  src/dx12_overlay.cpp
  src/ui.cpp
  src/material_names.cpp
  # the Modding Suite's shader analysis, shared as it is (the Material Editor's parameter names)
  tools/outfit_manager/material_params.cpp
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
)
if(MSVC)
  target_sources(rottr_framework PRIVATE
    src/material_parse_hook.asm
    src/eight_weight_patch_wrappers.asm
    src/swim_breath_hooks.asm
  )
endif()
set_target_properties(rottr_framework PROPERTIES OUTPUT_NAME "ROTTRFramework" PREFIX "" SUFFIX ".asi")
target_include_directories(rottr_framework PRIVATE src third_party ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(rottr_framework PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)


  target_compile_options(rottr_framework PRIVATE
    $<$<COMPILE_LANGUAGE:C,CXX>:/W4>
    $<$<COMPILE_LANGUAGE:C,CXX>:/EHsc>
    $<$<COMPILE_LANGUAGE:C,CXX>:/permissive->
  )
endif()
if(MSVC)
  set_source_files_properties(src/image_load.cpp PROPERTIES COMPILE_OPTIONS "/W0")   # stb_image
  set_source_files_properties(src/viewer_text.cpp PROPERTIES COMPILE_OPTIONS "/utf-8")   # its translations: UTF-8
  set_source_files_properties(src/cutscene_titles.cpp PROPERTIES COMPILE_OPTIONS "/utf-8")   # (the same)
endif()
target_link_libraries(rottr_framework PRIVATE lua54 minhook d3d11 d3d12 dxgi d3dcompiler user32 imm32 shell32 comdlg32 ole32
  mfplat mfreadwrite mfuuid)   # (v0.9.491: LaraFM's decoding)
add_custom_command(TARGET rottr_framework POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:rottr_framework> ${CMAKE_SOURCE_DIR}/package/ROTTRFramework.asi


  COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_SOURCE_DIR}/package/ROTTRFramework
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/package/ROTTRFramework
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/scripts ${CMAKE_SOURCE_DIR}/package/ROTTRFramework)
