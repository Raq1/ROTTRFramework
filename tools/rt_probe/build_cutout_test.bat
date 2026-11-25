@echo off
rem (v0.9.552) Builds cutout_test.exe: RtCutout's analysis over a folder of shader libraries
rem (cutout_test C:\TRTools\ROTTR\pcx64-w\sofia_cine.drm\ShaderLib).
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "%~dp0"
cl /nologo /EHsc /O2 /MT /std:c++17 /I ..\..\src cutout_test.cpp ..\..\src\rt_cutout.cpp /Fe:cutout_test.exe
