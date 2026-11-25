@echo off
rem (v0.9.543) Builds rtp_test.exe: RtProbeGpu::Probe on WARP (boxes and a ground plane), its view written to rtp_out.ppm
rem (python ppm2png.py rtp_out.ppm rtp_out.png 2 to look at it).
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "%~dp0"
cl /nologo /EHsc /O2 /MT /std:c++17 /I ..\..\src rtp_test.cpp ..\..\src\rt_probe_gpu.cpp /Fe:rtp_test.exe
