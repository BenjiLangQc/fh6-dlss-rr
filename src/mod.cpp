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

#define ImTextureID ImU64 // required by ReShade's overlay API, before imgui.h
#include <imgui.h>         // Dear ImGui 1.92.5 headers (external/imgui, the version ReShade 6.7.3 pins)
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
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Runs DLSS Ray Reconstruction instead of DLSS Super Resolution and feeds it FH6's raw ray-traced GI. F8 = mod on/off, F7 = RR, F6 = GI denoiser bypass, F5 = GI occlusion mode.";

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
	int gi_ao = 2;           // 0 = off, 1 = soft 0.25 m, 2 = soft 1 m (matches the game), 3 = hard 1.5 m
	int rr_preset = 0;       // RR model: 0 = game/driver default, 4 = preset D, 5 = preset E
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
				"; Raw GI occlusion from ray hit distance: 0 = off, 1 = soft 0.25 m, 2 = soft 1 m (matches the game), 3 = hard 1.5 m. F5 cycles\n"
				"gi_ao=2\n"
				"; Ray Reconstruction model: 0 = game/driver default (NVIDIA app override applies), 4 = preset D, 5 = preset E\n"
				"rr_preset=0\n"
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
	g_cfg.gi_ao = std::clamp(get(L"gi_ao", 2), 0, 3);
	g_cfg.rr_preset = get(L"rr_preset", 0);
	if (g_cfg.rr_preset != 4 && g_cfg.rr_preset != 5) g_cfg.rr_preset = 0;
	g_cfg.hotkeys = get(L"hotkeys", 1) != 0;
	g_cfg.toggle_key = std::clamp(get(L"toggle_key", VK_F8), 1, 254);
	logf("config: rr=%d gi_bypass=%d gi_scale=%d gi_ao=%d rr_preset=%d hotkeys=%d toggle_key=%d", g_cfg.rr, g_cfg.gi_bypass, g_cfg.gi_scale, g_cfg.gi_ao, g_cfg.rr_preset, g_cfg.hotkeys, g_cfg.toggle_key);
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

// The game's DLSS resource tags: only the depth is noted (diagnostics, see rr::on_dlss_depth); passed on unchanged.
static void note_dlss_tags(const sl::ResourceTag *tags, uint32_t n)
{
	for (uint32_t i = 0; tags && i < n; ++i)
		if (tags[i].type == sl::kBufferTypeDepth && tags[i].resource && tags[i].resource->native)
			rr::on_dlss_depth(static_cast<ID3D12Resource *>(tags[i].resource->native));
}

static sl::Result hk_slSetTagForFrame(const sl::FrameToken &frame, const sl::ViewportHandle &vp, const sl::ResourceTag *tags, uint32_t n, sl::CommandBuffer *cmd)
{
	note_dlss_tags(tags, n);
	return o_slSetTagForFrame(frame, vp, tags, n, cmd);
}

#pragma warning(push)
#pragma warning(disable : 4996) // slSetTag is deprecated, but the game may still use it
static PFun_slSetTag *o_slSetTag = nullptr;
static sl::Result hk_slSetTag(const sl::ViewportHandle &vp, const sl::ResourceTag *tags, uint32_t n, sl::CommandBuffer *cmd)
{
	note_dlss_tags(tags, n);
	return o_slSetTag(vp, tags, n, cmd);
}
#pragma warning(pop)

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
	// Tag hooks are diagnostics only: a failure must not count as a hook error, and the original stays usable.
	if (!hook_export(sl, "slSetTagForFrame", &hk_slSetTagForFrame, &o_slSetTagForFrame))
		o_slSetTagForFrame = reinterpret_cast<PFun_slSetTagForFrame *>(GetProcAddress(sl, "slSetTagForFrame"));
#pragma warning(suppress : 4996)
	if (GetProcAddress(sl, "slSetTag")) hook_export(sl, "slSetTag", &hk_slSetTag, &o_slSetTag);
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
		gi::on_pipeline(reinterpret_cast<ID3D12Device *>(dev->get_native()), reinterpret_cast<ID3D12RootSignature *>(layout.handle), gi::identity_hash(sd->code, sd->code_size), pipe.handle);
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
	if (!gi::enabled()) return false;
	ID3D12PipelineState *swap = nullptr;
	if (gi::on_dispatch(cmd->get_native(), &swap)) return true; // skipped (GI denoiser bypass)
	if (swap) reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native())->SetPipelineState(swap);
	return false;
}

static void on_reset_command_list(command_list *cmd)
{
	gi::on_reset(cmd->get_native());
}

static void on_bind_rts(command_list *cmd, uint32_t count, const resource_view *rtvs, resource_view dsv)
{
	// FH6 G-buffer: one 8-target MRT pass; RT3 = R32_UINT (normal+smoothness), RT4 = RGBA8 sRGB (albedo)
	if (count != 8 || !rtvs[3].handle || !rtvs[4].handle) return;
	device *dev = cmd->get_device();
	ID3D12Resource *rts[8] = {};
	for (uint32_t i = 0; i < 8; ++i)
		rts[i] = rtvs[i].handle ? reinterpret_cast<ID3D12Resource *>(dev->get_resource_from_view(rtvs[i]).handle) : nullptr;
	ID3D12Resource *depth = dsv.handle ? reinterpret_cast<ID3D12Resource *>(dev->get_resource_from_view(dsv).handle) : nullptr;
	if (rts[3] && rts[4] && rts[3]->GetDesc().Format == DXGI_FORMAT_R32_UINT && rts[4]->GetDesc().Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
		rr::on_gbuffer(rts, depth);
}

static bool g_f7_down = false, g_f6_down = false, g_f5_down = false, g_master_down = false;
// Panel debug view: show the raw 1-ray GI without RR denoising (RR off, bypass forced on). Never saved.
static std::atomic<bool> g_debug_raw_gi{false};
static bool g_debug_rr_before = true;

static void on_present(command_queue *, swapchain *, const rect *, const rect *, uint32_t, const rect *)
{
	const uint64_t frame = ++g_frame;
	try_hook_streamline();

	// The GI bypass only acts while RR succeeded within the last couple of frames
	// (GI passes are recorded before the RR evaluate of the same frame).
	// Debug view (panel): raw GI without RR is allowed on purpose.
	gi::set_allowed(g_debug_raw_gi.load() || (rr::enabled() && frame - g_last_rr_ok_frame.load() <= 2));

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
		const bool f5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
		if (f5 && !g_f5_down) gi::next_ao();
		g_f5_down = f5;
	}
}

// ---------------------------------------------------------------------------------------------
// Status panel (ReShade overlay > Add-ons > FH6 DLSS Ray Reconstruction)
// ---------------------------------------------------------------------------------------------

static void draw_settings(reshade::api::effect_runtime *)
{
	const ImVec4 green(0.45f, 0.85f, 0.45f, 1.0f), amber(1.0f, 0.75f, 0.3f, 1.0f), red(1.0f, 0.45f, 0.4f, 1.0f);
	const bool rr_running = rr::enabled() && g_frame.load() - g_last_rr_ok_frame.load() <= 2;
	const gi::Status st = gi::status();

	// Ray Reconstruction
	bool rr_on = rr::enabled();
	if (ImGui::Checkbox("DLSS Ray Reconstruction (F7)", &rr_on))
		rr::toggle();
	ImGui::SameLine();
	uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
	if (rr_running && rr::last_sizes(rw, rh, ow, oh))
		ImGui::TextColored(green, "running %ux%u -> %ux%u", rw, rh, ow, oh);
	else if (rr::enabled())
		ImGui::TextColored(amber, "not running: the game's DLSS is in use (DLSS off in game?)");
	else
		ImGui::TextDisabled("off: the game's DLSS");

	// RR model, switchable live for side-by-side comparisons
	int preset = rr::preset();
	ImGui::TextUnformatted("RR model");
	ImGui::SameLine();
	bool preset_changed = ImGui::RadioButton("Default (game / NVIDIA app)", &preset, 0);
	ImGui::SameLine();
	preset_changed |= ImGui::RadioButton("Preset D", &preset, 4);
	ImGui::SameLine();
	preset_changed |= ImGui::RadioButton("Preset E", &preset, 5);
	if (preset_changed)
	{
		rr::set_preset(preset);
		logf("RR model: %s (overlay)", preset == 0 ? "default (game / NVIDIA app)" : preset == 4 ? "preset D" : "preset E");
	}

	// Guide debug options: variants of the G-buffer decode for RR's guides, switchable live (never saved)
	{
		const uint32_t opts = rr::guide_options();
		int f0 = (int)(opts & rr::k_guide_f0_mask);
		if (f0 > 2) f0 = 0;
		bool full = (opts & rr::k_guide_full_diffuse) != 0;
		bool occ = (opts & rr::k_guide_rt5_occlusion) != 0;
		static const char *const f0_names[] = { "0.04 everywhere", "Game's rule", "RT6 (old)" };
		ImGui::TextUnformatted("Guide F0");
		bool c = false;
		for (int i = 0; i < 3; ++i)
		{
			ImGui::SameLine();
			c |= ImGui::RadioButton(f0_names[i], &f0, i);
		}
		ImGui::TextUnformatted("Guide debug");
		ImGui::SameLine();
		c |= ImGui::Checkbox("Diffuse without (1 - F0)", &full);
		ImGui::SameLine();
		c |= ImGui::Checkbox("Diffuse x RT5 occlusion", &occ);
		if (c)
		{
			const uint32_t n = (uint32_t)f0 | (full ? rr::k_guide_full_diffuse : 0) | (occ ? rr::k_guide_rt5_occlusion : 0);
			rr::set_guide_options(n);
			logf("RR guide options: F0 %s, diffuse without (1-F0) %s, diffuse x RT5 occlusion %s (overlay)",
				f0_names[f0], full ? "ON" : "off", occ ? "ON" : "off");
		}
	}

	ImGui::Separator();

	// GI denoiser bypass
	bool bypass = gi::enabled();
	if (ImGui::Checkbox("GI denoiser bypass (F6)", &bypass))
	{
		gi::set_enabled(bypass);
		gi::log_state("overlay");
	}
	ImGui::SameLine();
	if (!bypass)
		ImGui::TextDisabled("off: the game's own GI denoiser, RR upscales its result");
	else if (st.temporal_pipes == 0 || st.spatial_pipes == 0)
		ImGui::TextColored(red, "inactive: the game's GI shaders were not recognized (game update?) or RT GI is off");
	else if (!st.replacement_ready)
		ImGui::TextColored(red, "inactive: the replacement shader could not be created (see fh6rr.log)");
	else if (!st.allowed)
		ImGui::TextColored(amber, "waiting: only acts while Ray Reconstruction is running");
	else
		ImGui::TextColored(green, "active: RR denoises the raw ray-traced GI");

	// Occlusion of the raw GI, computed by our shader from the ray hit distance (unused while the bypass is off)
	ImGui::BeginDisabled(!bypass);
	int ao = gi::ao();
	ImGui::TextUnformatted("Raw GI occlusion (F5)");
	bool ao_changed = false;
	for (int i = 0; i < 4; ++i)
	{
		if (i) ImGui::SameLine();
		ao_changed |= ImGui::RadioButton(gi::ao_name(i), &ao, i);
	}
	if (ao_changed)
	{
		gi::set_ao(ao);
		logf("GI bypass AO: %s (overlay)", gi::ao_name(ao));
	}
	ImGui::EndDisabled();

	// Debug view: the raw GI as RR receives it, without RR's denoising
	ImGui::Separator();
	bool debug_raw = g_debug_raw_gi.load();
	if (ImGui::Checkbox("Debug: show raw GI without denoising", &debug_raw))
	{
		if (debug_raw)
		{
			g_debug_rr_before = rr::enabled();
			rr::set_enabled(false);
			gi::set_enabled(true);
		}
		else
			rr::set_enabled(g_debug_rr_before);
		g_debug_raw_gi = debug_raw;
		logf("debug view: raw GI without denoising %s (overlay)", debug_raw ? "ON (RR off, GI bypass forced on)" : "OFF");
	}
	if (debug_raw)
		ImGui::TextColored(amber, "RR is off: the game's DLSS upscaler still blends a few frames, so this is nearly raw.");

	ImGui::Separator();
	ImGui::TextDisabled("GI shaders found: temporal %zu, spatial %zu   |   F8: whole mod on/off   |   Settings: fh6rr.ini",
		st.temporal_pipes, st.spatial_pipes);
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
		rr::set_preset(g_cfg.rr_preset);
		gi::set_enabled(g_cfg.gi_bypass);
		gi::set_scale(g_cfg.gi_scale);
		gi::set_ao(g_cfg.gi_ao);

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
		reshade::register_overlay(nullptr, draw_settings); // drawn in the add-on's entry in ReShade's Add-ons tab
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
