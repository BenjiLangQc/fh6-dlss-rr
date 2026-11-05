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

echo Dependencies ready.
