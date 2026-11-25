@echo off
rem (v0.9.543) Compiles src/rt_probe.hlsl (the ray tracing probe's debug view) to DXIL into src/rt_probe_cs.inc;
rem (v0.9.545) and src/rt_probe_skin.hlsl (its skinning) into src/rt_probe_skin_cs.inc;
rem (v0.9.552) and src/rt_probe_uv.hlsl (the cutouts' UV gather) into src/rt_probe_uv_cs.inc.
rem (v0.9.560) and src/rt_probe_shadow.hlsl (the lamp shadow mask) into src/rt_probe_shadow_cs.inc.
rem (v0.9.564) and src/rt_probe_blur.hlsl (the mask's blur) into src/rt_probe_blur_cs.inc.
rem Needs the Windows SDK's dxc.exe (10.0.26100 or later: cs_6_5, inline ray queries) with dxil.dll beside it.
set DXC=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe
cd /d "%~dp0..\.."
"%DXC%" -nologo -T cs_6_5 -E main -O3 -Fh src\rt_probe_cs.inc -Vn kRtProbeCs src\rt_probe.hlsl
"%DXC%" -nologo -T cs_6_0 -E main -O3 -Fh src\rt_probe_skin_cs.inc -Vn kRtProbeSkinCs src\rt_probe_skin.hlsl
"%DXC%" -nologo -T cs_6_0 -E main -O3 -Fh src\rt_probe_uv_cs.inc -Vn kRtProbeUvCs src\rt_probe_uv.hlsl
"%DXC%" -nologo -T cs_6_5 -E main -O3 -Fh src\rt_probe_shadow_cs.inc -Vn kRtProbeShadowCs src\rt_probe_shadow.hlsl
"%DXC%" -nologo -T cs_6_0 -E main -O3 -Fh src\rt_probe_blur_cs.inc -Vn kRtProbeBlurCs src\rt_probe_blur.hlsl
