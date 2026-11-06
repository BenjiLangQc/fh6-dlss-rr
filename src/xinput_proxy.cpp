// Proxy xinput1_3.dll for the standalone build. FH6 imports xinput1_3 statically (ordinals 2 and 4), so a copy
// placed next to forzahorizon6.exe loads at process start. Every export is forwarded, by ordinal, to the real
// System32 copy (loaded by full path so it is a distinct module from this one). See xinput1_3.def.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void fh6rr_standalone_init(HMODULE self);
void fh6rr_standalone_shutdown();

static HMODULE g_real = nullptr;

static FARPROC real_proc(WORD ordinal)
{
	if (!g_real)
	{
		wchar_t path[MAX_PATH];
		const UINT n = GetSystemDirectoryW(path, MAX_PATH);
		if (n == 0 || n > MAX_PATH - 20) return nullptr;
		lstrcatW(path, L"\\xinput1_3.dll");
		g_real = LoadLibraryW(path);
		if (!g_real) return nullptr;
	}
	return GetProcAddress(g_real, MAKEINTRESOURCEA(ordinal));
}

template <typename R, typename... A>
static R forward(WORD ordinal, R fallback, A... args)
{
	using Fn = R(WINAPI *)(A...);
	const auto fn = reinterpret_cast<Fn>(real_proc(ordinal));
	return fn ? fn(args...) : fallback;
}

// Signatures from xinput.h (pointer arguments kept opaque so the real header is not needed).
extern "C"
{
	DWORD WINAPI proxy_XInputGetState(DWORD user, void *state) { return forward<DWORD>(2, ERROR_DEVICE_NOT_CONNECTED, user, state); }
	DWORD WINAPI proxy_XInputSetState(DWORD user, void *vibration) { return forward<DWORD>(3, ERROR_DEVICE_NOT_CONNECTED, user, vibration); }
	DWORD WINAPI proxy_XInputGetCapabilities(DWORD user, DWORD flags, void *caps) { return forward<DWORD>(4, ERROR_DEVICE_NOT_CONNECTED, user, flags, caps); }
	void WINAPI proxy_XInputEnable(BOOL enable)
	{
		using Fn = void(WINAPI *)(BOOL);
		if (const auto fn = reinterpret_cast<Fn>(real_proc(5))) fn(enable);
	}
	DWORD WINAPI proxy_XInputGetDSoundAudioDeviceGuids(DWORD user, GUID *render, GUID *capture) { return forward<DWORD>(6, ERROR_DEVICE_NOT_CONNECTED, user, render, capture); }
	DWORD WINAPI proxy_XInputGetBatteryInformation(DWORD user, BYTE type, void *info) { return forward<DWORD>(7, ERROR_DEVICE_NOT_CONNECTED, user, type, info); }
	DWORD WINAPI proxy_XInputGetKeystroke(DWORD user, DWORD reserved, void *keystroke) { return forward<DWORD>(8, ERROR_DEVICE_NOT_CONNECTED, user, reserved, keystroke); }
	// Undocumented ordinal-only exports
	DWORD WINAPI proxy_Ordinal100(DWORD user, void *state) { return forward<DWORD>(100, ERROR_DEVICE_NOT_CONNECTED, user, state); }            // XInputGetStateEx
	DWORD WINAPI proxy_Ordinal101(DWORD user, DWORD flags, void *p) { return forward<DWORD>(101, ERROR_DEVICE_NOT_CONNECTED, user, flags, p); } // XInputWaitForGuideButton
	DWORD WINAPI proxy_Ordinal102(DWORD user) { return forward<DWORD>(102, ERROR_DEVICE_NOT_CONNECTED, user); }                                // XInputCancelGuideButtonWait
	DWORD WINAPI proxy_Ordinal103(DWORD user) { return forward<DWORD>(103, ERROR_DEVICE_NOT_CONNECTED, user); }                                // XInputPowerOffController
	// The real DLL exports its DllMain at ordinal 1; keep the slot occupied with a harmless stub.
	BOOL WINAPI proxy_DllMainExport(HINSTANCE, DWORD, LPVOID) { return TRUE; }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(module);
		fh6rr_standalone_init(module);
		break;
	case DLL_PROCESS_DETACH:
		fh6rr_standalone_shutdown();
		break;
	}
	return TRUE;
}
