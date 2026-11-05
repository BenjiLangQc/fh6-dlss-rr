// FH6 DLSS Ray Reconstruction mod (ReShade 6.x add-on, D3D12).
//
// What it does (see notes/phase2_findings.md for how each piece was worked out):
//   1. Adds DLSS Ray Reconstruction (Streamline dlss_d, shipped with the game) to slInit's feature list.
//   2. Replaces the game's DLSS-SR evaluate with: guide pass (albedo / specular albedo / normal+roughness decoded
//      from FH6's G-buffer) + DLSS-RR evaluate.                                                    [rr.cpp]
//   3. While RR is working, bypasses FH6's RT GI denoiser so RR receives the raw 1-ray GI.          [gi_bypass.cpp]
//
// Requires ReShade.ini:  [ADDON]  LoadFromDllMain=fh6rr.addon64   (so slInit can be hooked).
// Never registers a vectored exception handler (FH6 relies on guard-page exceptions).

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

#include <reshade.hpp>
#include <MinHook.h>

#include "rr.h"
#include "gi_bypass.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using namespace reshade::api;

extern "C" __declspec(dllexport) const char *NAME = "FH6 DLSS Ray Reconstruction";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Runs DLSS Ray Reconstruction instead of DLSS Super Resolution and feeds it FH6's raw ray-traced GI. F7 = RR on/off, F6 = GI denoiser bypass on/off.";

// ---------------------------------------------------------------------------------------------
// Log + config
// ---------------------------------------------------------------------------------------------

static std::filesystem::path g_dir;
static std::mutex g_log_mtx;
static FILE *g_log = nullptr;

void logf(const char *fmt, ...)
{
	std::lock_guard lock(g_log_mtx);
	if (!g_log) return;
	SYSTEMTIME st; GetLocalTime(&st);
	fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	va_list args; va_start(args, fmt);
	vfprintf(g_log, fmt, args);
	va_end(args);
	fputc('\n', g_log);
	fflush(g_log);
}

struct Config
{
	bool rr = true;          // start with DLSS-RR enabled
	bool gi_bypass = true;   // start with the GI denoiser bypass enabled (only acts while RR works)
	int gi_scale = 0;        // 0 = x1 (matches the game), 1 = xPI, 2 = x2, 3 = x0.5
	bool hotkeys = true;     // F7 / F6 toggles
	int toggle_key = VK_F8;  // master on/off key (whole mod)
};
static Config g_cfg;

static void load_config()
{
	const auto ini = g_dir / "fh6rr.ini";
	if (!std::filesystem::exists(ini))
	{
		if (FILE *f = _wfopen(ini.c_str(), L"w"))
		{
			fputs("; FH6 DLSS Ray Reconstruction mod\n"
				"[fh6rr]\n"
				"; Start with DLSS Ray Reconstruction on (1) or off (0). F7 toggles in game.\n"
				"rr=1\n"
				"; Bypass the game's RT GI denoiser so RR denoises the raw GI (only while RR works). F6 toggles.\n"
				"gi_bypass=1\n"
				"; Raw GI brightness: 0 = x1 (matches the game), 1 = xPI, 2 = x2, 3 = x0.5\n"
				"gi_scale=0\n"
				"; Enable the F7/F6 hotkeys\n"
				"hotkeys=1\n"
				"; Master key that switches the whole mod on/off (Windows virtual-key code). 119 = F8, 120 = F9, 121 = F10\n"
				"toggle_key=119\n", f);
			fclose(f);
		}
	}
	auto get = [&](const wchar_t *key, int def) { return (int)GetPrivateProfileIntW(L"fh6rr", key, def, ini.c_str()); };
	g_cfg.rr = get(L"rr", 1) != 0;
	g_cfg.gi_bypass = get(L"gi_bypass", 1) != 0;
	g_cfg.gi_scale = std::clamp(get(L"gi_scale", 0), 0, 3);
	g_cfg.hotkeys = get(L"hotkeys", 1) != 0;
	g_cfg.toggle_key = std::clamp(get(L"toggle_key", VK_F8), 1, 254);
	logf("config: rr=%d gi_bypass=%d gi_scale=%d hotkeys=%d toggle_key=%d", g_cfg.rr, g_cfg.gi_bypass, g_cfg.gi_scale, g_cfg.hotkeys, g_cfg.toggle_key);
}

static uint64_t fnv1a(const void *data, size_t size)
{
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < size; ++i) { h ^= static_cast<const uint8_t *>(data)[i]; h *= 1099511628211ull; }
	return h;
}

// ---------------------------------------------------------------------------------------------
// Native command list resolution (the game hands Streamline a ReShade/SL-wrapped list)
// ---------------------------------------------------------------------------------------------

static std::mutex g_cl_mtx;
static std::unordered_map<uint64_t, uint64_t> g_cl_to_native; // reshade command_list* -> native list
static std::set<uint64_t> g_native_lists;

static void on_init_command_list(command_list *cmd)
{
	std::lock_guard lock(g_cl_mtx);
	g_cl_to_native[reinterpret_cast<uint64_t>(cmd)] = cmd->get_native();
	g_native_lists.insert(cmd->get_native());
}

static void on_destroy_command_list(command_list *cmd)
{
	std::lock_guard lock(g_cl_mtx);
	auto it = g_cl_to_native.find(reinterpret_cast<uint64_t>(cmd));
	if (it != g_cl_to_native.end()) { g_native_lists.erase(it->second); g_cl_to_native.erase(it); }
}

static PFun_slGetNativeInterface *s_slGetNativeInterface = nullptr;

static ID3D12GraphicsCommandList *resolve_native_list(void *cmd)
{
	if (!cmd) return nullptr;
	auto lookup = [&](uint64_t p) -> uint64_t {
		std::lock_guard lock(g_cl_mtx);
		if (g_native_lists.count(p)) return p;
		for (int off = 0; off <= 64; off += 8) // ReShade proxy: command_list base a few pointers into the COM object
		{
			auto it = g_cl_to_native.find(p + off);
			if (it != g_cl_to_native.end()) return it->second;
		}
		return 0;
	};
	if (uint64_t n = lookup(reinterpret_cast<uint64_t>(cmd))) return reinterpret_cast<ID3D12GraphicsCommandList *>(n);
	if (s_slGetNativeInterface)
	{
		void *base = nullptr;
		if (s_slGetNativeInterface(cmd, &base) == sl::Result::eOk && base)
			if (uint64_t n = lookup(reinterpret_cast<uint64_t>(base))) return reinterpret_cast<ID3D12GraphicsCommandList *>(n);
	}
	return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Streamline hooks
// ---------------------------------------------------------------------------------------------

static PFun_slInit *o_slInit = nullptr;
static PFun_slSetTagForFrame *o_slSetTagForFrame = nullptr;
static PFun_slEvaluateFeature *o_slEvaluateFeature = nullptr;
static PFun_slSetConstants *o_slSetConstants = nullptr;
static PFun_slGetFeatureFunction *o_slGetFeatureFunction = nullptr;
static PFun_slIsFeatureLoaded *o_slIsFeatureLoaded = nullptr;
static PFun_slDLSSSetOptions *o_slDLSSSetOptions = nullptr;

static std::atomic<uint64_t> g_frame{0};
static std::atomic<uint64_t> g_last_rr_ok_frame{0};

static sl::Result hk_slInit(const sl::Preferences &pref, uint64_t sdk_version)
{
	if (!pref.featuresToLoad || !pref.numFeaturesToLoad)
	{
		logf("slInit: game loads all features");
		return o_slInit(pref, sdk_version);
	}
	static std::vector<sl::Feature> features;
	features.assign(pref.featuresToLoad, pref.featuresToLoad + pref.numFeaturesToLoad);
	if (std::find(features.begin(), features.end(), sl::kFeatureDLSS_RR) == features.end())
		features.push_back(sl::kFeatureDLSS_RR);
	sl::Preferences p = pref;
	p.featuresToLoad = features.data();
	p.numFeaturesToLoad = (uint32_t)features.size();
	const sl::Result r = o_slInit(p, sdk_version);
	logf("slInit: added DLSS Ray Reconstruction to %u game features -> %d", pref.numFeaturesToLoad, (int)r);
	return r;
}

static sl::Result hk_slDLSSSetOptions(const sl::ViewportHandle &vp, const sl::DLSSOptions &o)
{
	rr::on_dlss_options(o);
	return o_slDLSSSetOptions(vp, o);
}

// Wrap slDLSSSetOptions by handing the game our wrapper (never detour plugin functions: they share code).
static sl::Result hk_slGetFeatureFunction(sl::Feature feature, const char *name, void *&fn)
{
	const sl::Result r = o_slGetFeatureFunction(feature, name, fn);
	if (r == sl::Result::eOk && fn && name && feature == sl::kFeatureDLSS && strcmp(name, "slDLSSSetOptions") == 0)
	{
		o_slDLSSSetOptions = reinterpret_cast<PFun_slDLSSSetOptions *>(fn);
		fn = reinterpret_cast<void *>(&hk_slDLSSSetOptions);
	}
	return r;
}

static sl::Result hk_slSetConstants(const sl::Constants &c, const sl::FrameToken &frame, const sl::ViewportHandle &vp)
{
	if (c.structVersion >= 2) rr::on_constants(c);
	return o_slSetConstants(c, frame, vp);
}

static sl::Result hk_slEvaluateFeature(sl::Feature feature, const sl::FrameToken &frame, const sl::BaseStructure **inputs, uint32_t num, sl::CommandBuffer *cmd)
{
	if (feature == sl::kFeatureDLSS && rr::enabled())
	{
		if (rr::evaluate(frame, inputs, num, cmd, resolve_native_list(cmd)))
		{
			g_last_rr_ok_frame = g_frame.load();
			return sl::Result::eOk; // RR replaced DLSS-SR this frame
		}
	}
	return o_slEvaluateFeature(feature, frame, inputs, num, cmd);
}

template <typename T>
static bool hook_export(HMODULE mod, const char *name, T *detour, T **original)
{
	void *target = reinterpret_cast<void *>(GetProcAddress(mod, name));
	if (!target || MH_CreateHook(target, reinterpret_cast<void *>(detour), reinterpret_cast<void **>(original)) != MH_OK || MH_EnableHook(target) != MH_OK)
	{
		logf("failed to hook %s", name);
		return false;
	}
	return true;
}

static std::atomic<bool> g_sl_hooked{false};
static std::mutex g_hook_mtx;

static void try_hook_streamline()
{
	if (g_sl_hooked.load()) return;
	std::lock_guard lock(g_hook_mtx);
	if (g_sl_hooked.load()) return;
	HMODULE sl = GetModuleHandleW(L"sl.interposer.dll");
	if (!sl) return;

	bool ok = hook_export(sl, "slInit", &hk_slInit, &o_slInit);
	ok &= hook_export(sl, "slEvaluateFeature", &hk_slEvaluateFeature, &o_slEvaluateFeature);
	ok &= hook_export(sl, "slSetConstants", &hk_slSetConstants, &o_slSetConstants);
	ok &= hook_export(sl, "slGetFeatureFunction", &hk_slGetFeatureFunction, &o_slGetFeatureFunction);
	o_slSetTagForFrame = reinterpret_cast<PFun_slSetTagForFrame *>(GetProcAddress(sl, "slSetTagForFrame"));
	o_slIsFeatureLoaded = reinterpret_cast<PFun_slIsFeatureLoaded *>(GetProcAddress(sl, "slIsFeatureLoaded"));
	s_slGetNativeInterface = reinterpret_cast<PFun_slGetNativeInterface *>(GetProcAddress(sl, "slGetNativeInterface"));

	rr::SlFunctions fns;
	fns.set_tag_for_frame = o_slSetTagForFrame;
	fns.evaluate = o_slEvaluateFeature;
	fns.get_feature_function = o_slGetFeatureFunction;
	fns.is_feature_loaded = o_slIsFeatureLoaded;
	rr::set_sl(fns);

	g_sl_hooked = true;
	logf("Streamline hooked (%s)", ok ? "ok" : "with errors");
}

using PFN_LoadLibraryExW = HMODULE(WINAPI *)(LPCWSTR, HANDLE, DWORD);
static PFN_LoadLibraryExW o_LoadLibraryExW = nullptr;
static HMODULE WINAPI hk_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
{
	HMODULE m = o_LoadLibraryExW(name, file, flags);
	if (m && name && !g_sl_hooked.load() && wcsstr(name, L"sl.interposer"))
		try_hook_streamline();
	return m;
}

// ---------------------------------------------------------------------------------------------
// ReShade events
// ---------------------------------------------------------------------------------------------

static void on_init_device(device *dev)
{
	rr::on_device(reinterpret_cast<ID3D12Device *>(dev->get_native()));
	try_hook_streamline();
}

static void on_init_pipeline(device *dev, pipeline_layout layout, uint32_t count, const pipeline_subobject *subobjects, pipeline pipe)
{
	for (uint32_t i = 0; i < count; ++i)
	{
		if (subobjects[i].type != pipeline_subobject_type::compute_shader || !subobjects[i].data) continue;
		const auto *sd = static_cast<const shader_desc *>(subobjects[i].data);
		if (!sd->code || !sd->code_size) continue;
		gi::on_pipeline(reinterpret_cast<ID3D12Device *>(dev->get_native()), reinterpret_cast<ID3D12RootSignature *>(layout.handle), fnv1a(sd->code, sd->code_size), pipe.handle);
	}
}

static void on_destroy_pipeline(device *, pipeline pipe)
{
	gi::on_destroy_pipeline(pipe.handle);
}

static void on_bind_pipeline(command_list *cmd, pipeline_stage stages, pipeline pipe)
{
	if ((static_cast<uint32_t>(stages) & static_cast<uint32_t>(pipeline_stage::compute_shader)) != 0)
		gi::on_bind_compute(cmd->get_native(), pipe.handle);
}

static bool on_dispatch(command_list *cmd, uint32_t, uint32_t, uint32_t)
{
	return gi::enabled() && gi::on_dispatch(cmd->get_native(), reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native()));
}

static void on_reset_command_list(command_list *cmd)
{
	gi::on_reset(cmd->get_native());
}

static void on_bind_rts(command_list *cmd, uint32_t count, const resource_view *rtvs, resource_view)
{
	// FH6 G-buffer: one 8-target MRT pass; RT3 = R32_UINT (normal+smoothness), RT4 = RGBA8 sRGB (albedo)
	if (count != 8 || !rtvs[3].handle || !rtvs[4].handle) return;
	device *dev = cmd->get_device();
	ID3D12Resource *rts[8] = {};
	for (uint32_t i = 0; i < 8; ++i)
		rts[i] = rtvs[i].handle ? reinterpret_cast<ID3D12Resource *>(dev->get_resource_from_view(rtvs[i]).handle) : nullptr;
	if (rts[3] && rts[4] && rts[3]->GetDesc().Format == DXGI_FORMAT_R32_UINT && rts[4]->GetDesc().Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
		rr::on_gbuffer(rts);
}

static bool g_f7_down = false, g_f6_down = false, g_master_down = false;

static void on_present(command_queue *, swapchain *, const rect *, const rect *, uint32_t, const rect *)
{
	const uint64_t frame = ++g_frame;
	try_hook_streamline();

	// The GI bypass only acts while RR succeeded within the last couple of frames
	// (GI passes are recorded before the RR evaluate of the same frame).
	gi::set_allowed(rr::enabled() && frame - g_last_rr_ok_frame.load() <= 2);

	if (g_cfg.hotkeys)
	{
		// Master key: whole mod on/off. Off = the game's own DLSS and GI denoiser.
		const bool master = (GetAsyncKeyState(g_cfg.toggle_key) & 0x8000) != 0;
		if (master && !g_master_down)
		{
			const bool on = !(rr::enabled() || gi::enabled());
			rr::set_enabled(on);
			gi::set_enabled(on);
			logf("mod %s (master key)", on ? "ON" : "OFF");
		}
		g_master_down = master;
		const bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
		if (f7 && !g_f7_down) rr::toggle();
		g_f7_down = f7;
		const bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
		if (f6 && !g_f6_down) gi::toggle();
		g_f6_down = f6;
	}
}

// ---------------------------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
	{
		if (!reshade::register_addon(module))
			return FALSE;

		wchar_t path[MAX_PATH] = {};
		GetModuleFileNameW(module, path, MAX_PATH);
		g_dir = std::filesystem::path(path).parent_path();
		g_log = _wfopen((g_dir / "fh6rr.log").c_str(), L"w");
		logf("FH6 DLSS Ray Reconstruction mod loaded");
		load_config();
		rr::set_enabled(g_cfg.rr);
		gi::set_enabled(g_cfg.gi_bypass);
		gi::set_scale(g_cfg.gi_scale);

		MH_Initialize();
		if (HMODULE kb = GetModuleHandleW(L"kernelbase.dll"))
		{
			void *ll = reinterpret_cast<void *>(GetProcAddress(kb, "LoadLibraryExW"));
			if (ll && MH_CreateHook(ll, &hk_LoadLibraryExW, reinterpret_cast<void **>(&o_LoadLibraryExW)) == MH_OK)
				MH_EnableHook(ll);
		}
		try_hook_streamline(); // in case Streamline is already loaded

		reshade::register_event<reshade::addon_event::init_device>(on_init_device);
		reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
		reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
		reshade::register_event<reshade::addon_event::init_command_list>(on_init_command_list);
		reshade::register_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
		reshade::register_event<reshade::addon_event::reset_command_list>(on_reset_command_list);
		reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
		reshade::register_event<reshade::addon_event::dispatch>(on_dispatch);
		reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_rts);
		reshade::register_event<reshade::addon_event::present>(on_present);
		break;
	}
	case DLL_PROCESS_DETACH:
		reshade::unregister_addon(module);
		MH_DisableHook(MH_ALL_HOOKS);
		MH_Uninitialize();
		if (g_log) { fclose(g_log); g_log = nullptr; }
		break;
	}
	return TRUE;
}
