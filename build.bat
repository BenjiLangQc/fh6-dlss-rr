@echo off
setlocal
rem Builds (x64 Release, VS 2022 Build Tools):
rem   build\mod\fh6rr.addon64          - the DLSS Ray Reconstruction mod (install this one)
rem   build\recon\fh6rr_recon.addon64  - the analysis/recon tool (captures, debug views). Never install both.
rem The standalone (no ReShade) version lives in its own repo: BenjiLangQc/fh6-dlss-rr-standalone.
set ROOT=%~dp0
set VSDEV="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VSDEV% set VSDEV="C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call %VSDEV% >nul || exit /b 1

set B=%ROOT%build
if not exist "%B%\gen" mkdir "%B%\gen"
if not exist "%B%\mod" mkdir "%B%\mod"
if not exist "%B%\recon" mkdir "%B%\recon"

rem ---- shaders -> headers
set DXC="C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe"
set SH=%ROOT%src\shaders
%DXC% -nologo -T cs_6_0 -E main -O3 -Fh "%B%\gen\debug_view_cs.h" -Vn g_debug_view_cs "%SH%\debug_view.hlsl" >nul || goto :fail
%DXC% -nologo -T cs_6_0 -E main -O3 -Fh "%B%\gen\rr_guides_cs.h" -Vn g_rr_guides_cs "%SH%\rr_guides.hlsl" >nul || goto :fail
rem raw-GI replacement: 4 brightness scales x 4 AO modes
echo #pragma once> "%B%\gen\gi_raw_cs.h"
for %%S in (0 1 2 3) do for %%A in (0 1 2 3) do (
	%DXC% -nologo -T cs_6_0 -E main -O3 -D GI_SCALE_IDX=%%S -D GI_AO_IDX=%%A -Fh "%B%\gen\gi_raw_s%%S_a%%A.h" -Vn g_gi_raw_s%%S_a%%A "%SH%\gi_raw.hlsl" >nul || goto :fail
	echo #include "gi_raw_s%%S_a%%A.h">> "%B%\gen\gi_raw_cs.h"
)

set INC=/I"%B%\gen" /I"%ROOT%src" /I"%ROOT%external\reshade\include" /I"%ROOT%external\reshade\examples\utils" /I"%ROOT%external\streamline\include" /I"%ROOT%external\minhook\include"
set CXXFLAGS=/nologo /c /O2 /MT /W3 /std:c++20 /EHsc /permissive- /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE
set MH=%ROOT%external\minhook\src
set MHSRC="%MH%\hook.c" "%MH%\buffer.c" "%MH%\trampoline.c" "%MH%\hde\hde64.c"
set LIBS=d3d12.lib dxgi.lib dxguid.lib user32.lib

rem ---- mod
pushd "%B%\mod"
del /q *.obj 2>nul
cl /nologo /c /O2 /MT /W3 %INC% %MHSRC% >nul || goto :failpop
cl %CXXFLAGS% %INC% "%ROOT%src\mod.cpp" "%ROOT%src\rr.cpp" "%ROOT%src\gi_bypass.cpp" || goto :failpop
link /nologo /DLL /MAP:fh6rr.map /OUT:fh6rr.addon64 *.obj %LIBS% >nul || goto :failpop
popd

rem ---- recon tool
pushd "%B%\recon"
del /q *.obj 2>nul
cl /nologo /c /O2 /MT /W3 %INC% %MHSRC% >nul || goto :failpop
cl %CXXFLAGS% %INC% "%ROOT%src\recon.cpp" "%ROOT%src\debug_view.cpp" "%ROOT%src\rr.cpp" "%ROOT%src\gi_bypass.cpp" "%ROOT%external\reshade\examples\utils\descriptor_tracking.cpp" "%ROOT%external\reshade\examples\utils\state_tracking.cpp" >nul || goto :failpop
link /nologo /DLL /MAP:fh6rr_recon.map /OUT:fh6rr_recon.addon64 *.obj %LIBS% >nul || goto :failpop
popd

echo Built: %B%\mod\fh6rr.addon64
echo Built: %B%\recon\fh6rr_recon.addon64
exit /b 0

:failpop
popd
:fail
echo BUILD FAILED
exit /b 1
