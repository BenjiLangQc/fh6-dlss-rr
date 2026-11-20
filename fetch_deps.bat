@echo off
rem Fetches the third-party sources build.bat needs, at the versions the mod was built and tested with.
rem (Streamline headers are already committed under external\streamline.)
setlocal
set EXT=%~dp0external
if not exist "%EXT%" mkdir "%EXT%"

if not exist "%EXT%\reshade\include\reshade.hpp" (
	git clone --filter=blob:none --sparse --branch v6.7.3 --depth 1 https://github.com/crosire/reshade "%EXT%\reshade" || exit /b 1
	git -C "%EXT%\reshade" sparse-checkout set include examples/utils || exit /b 1
)

if not exist "%EXT%\minhook\include\MinHook.h" (
	git clone https://github.com/TsudaKageyu/minhook "%EXT%\minhook" || exit /b 1
	git -C "%EXT%\minhook" checkout 8af6b4acae5a9388fd742b56fa79ece89d96f823 || exit /b 1
)

rem Dear ImGui headers for the ReShade overlay panel, at the commit ReShade v6.7.3 pins (1.92.5, IMGUI_VERSION_NUM 19250)
if not exist "%EXT%\imgui\imgui.h" (
	git init -q "%EXT%\imgui" || exit /b 1
	git -C "%EXT%\imgui" remote add origin https://github.com/ocornut/imgui.git || exit /b 1
	git -C "%EXT%\imgui" fetch -q --depth 1 --filter=blob:none origin 3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c || exit /b 1
	git -C "%EXT%\imgui" sparse-checkout set --no-cone /imgui.h /imconfig.h || exit /b 1
	git -C "%EXT%\imgui" checkout -q FETCH_HEAD || exit /b 1
)

echo Dependencies ready.
