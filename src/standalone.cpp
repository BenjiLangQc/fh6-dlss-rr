// FH6 DLSS Ray Reconstruction - standalone build (no ReShade).
//
// Loaded as a proxy xinput1_3.dll from the game folder (FH6 imports it statically, so it loads at process
// start, before Streamline initializes). Calls into the real System32 xinput1_3.dll are forwarded by
// xinput_proxy.cpp. Everything ReShade provided in the add-on build is done here with native D3D12 hooks:
//   - slUpgradeInterface / slSetD3DDevice         -> the game's D3D12 device (never patch d3d12.dll itself:
//                                                    an inline hook on D3D12CreateDevice stopped Streamline
//                                                    from loading its plugins and broke RTSS's overlay)
//   - device: CreateComputePipelineState / CreatePipelineState / CreatePipelineLibrary(+Load*) -> shader hashes
//   - device: CreateRenderTargetView               -> RTV handle -> resource (to find the G-buffer)
//   - command list: Reset / SetPipelineState / Dispatch / OMSetRenderTargets (every distinct implementation)
// The Streamline hooks, RR (rr.cpp) and the GI bypass (gi_bypass.cpp) are shared with the add-on build.
// Never registers a vectored exception handler (FH6 relies on guard-page exceptions).

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <MinHook.h>

#include "rr.h"
#include "gi_bypass.h"
#include "gi_raw_cs.h" // generated: g_gi_conv_s<scale>_a<ao>, g_gi_copy_cs

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------------------------
// Log + config (same fh6rr.ini format as the add-on build)
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
	bool rr = true;
	bool gi_bypass = true;
	int gi_scale = 0;
	int gi_ao = 2;
	bool hook_device = true; // diagnostics: D3D12 device hooks (pipeline creation, RTVs)
	bool hook_lists = true;  // diagnostics: D3D12 command list hooks (bypass dispatches, G-buffer binds)
};
static Config g_cfg;
static bool g_swap_gi = false; // standalone GI bypass: swap the GI shaders at pipeline creation (set at launch)

static void load_config()
{
	const auto ini = g_dir / "fh6rr.ini";
	if (!std::filesystem::exists(ini))
	{
		if (FILE *f = _wfopen(ini.c_str(), L"w"))
		{
			fputs("; FH6 DLSS Ray Reconstruction mod\n"
				"[fh6rr]\n"
				"; Standalone version: settings are read at launch - restart the game after changing them.\n"
				"; DLSS Ray Reconstruction on (1) or off (0)\n"
				"rr=1\n"
				"; Bypass the game's RT GI denoiser so RR denoises the raw GI (needs rr=1)\n"
				"gi_bypass=1\n"
				"; Raw GI brightness: 0 = x1 (matches the game), 1 = xPI, 2 = x2, 3 = x0.5\n"
				"gi_scale=0\n"
				"; Raw GI occlusion from ray hit distance: 0 = off, 1 = soft 0.25 m, 2 = soft 1 m (matches the game), 3 = hard 1.5 m\n"
				"gi_ao=2\n", f);
			fclose(f);
		}
	}
	auto get = [&](const wchar_t *key, int def) { return (int)GetPrivateProfileIntW(L"fh6rr", key, def, ini.c_str()); };
	g_cfg.rr = get(L"rr", 1) != 0;
	g_cfg.gi_bypass = get(L"gi_bypass", 1) != 0;
	g_cfg.gi_scale = std::clamp(get(L"gi_scale", 0), 0, 3);
	g_cfg.gi_ao = std::clamp(get(L"gi_ao", 2), 0, 3);
	g_cfg.hook_device = get(L"hook_device", 1) != 0;
	g_cfg.hook_lists = get(L"hook_lists", 1) != 0;
	g_swap_gi = g_cfg.gi_bypass && g_cfg.rr;
	logf("config: rr=%d gi_bypass=%d gi_scale=%d gi_ao=%d hook_device=%d hook_lists=%d (no hotkeys in the standalone version: restart to apply changes)",
		g_cfg.rr, g_cfg.gi_bypass, g_cfg.gi_scale, g_cfg.gi_ao, g_cfg.hook_device, g_cfg.hook_lists);
}

static uint64_t fnv1a(const void *data, size_t size)
{
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < size; ++i) { h ^= static_cast<const uint8_t *>(data)[i]; h *= 1099511628211ull; }
	return h;
}

// ---------------------------------------------------------------------------------------------
// Hook helpers: command list methods differ per list type, so each distinct address gets its own
// templated detour + trampoline slot.
// ---------------------------------------------------------------------------------------------

static std::string module_of(const void *addr)
{
	HMODULE mod = nullptr;
	wchar_t path[MAX_PATH] = L"?";
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(addr), &mod) && mod)
		GetModuleFileNameW(mod, path, MAX_PATH);
	const wchar_t *base = wcsrchr(path, L'\\');
	char out[MAX_PATH] = {};
	WideCharToMultiByte(CP_UTF8, 0, base ? base + 1 : path, -1, out, MAX_PATH - 1, nullptr, nullptr);
	return out;
}

// True when obj's vtable lives in the D3D12 runtime (a native object, not a Streamline/overlay wrapper).
static bool is_runtime_object(void *obj)
{
	static HMODULE d3d12core = GetModuleHandleW(L"D3D12Core.dll");
	if (!obj || !d3d12core) return false;
	HMODULE mod = nullptr;
	void *vt = *reinterpret_cast<void **>(obj);
	return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(vt), &mod) && mod == d3d12core;
}

constexpr int kVariants = 8;
static std::mutex g_hook_mtx;

// Command list vtables differ per list type: each distinct vtable slot gets its own templated detour + saved
// original. Slots are redirected (VirtualProtect + pointer write); D3D12's code is never modified, so RTSS and
// other overlays that code-hook these functions keep working.
static void hook_slot_variant(void **vt, int index, void *const *detours, void **originals, std::map<void *, int> &seen, const char *name)
{
	std::lock_guard lock(g_hook_mtx);
	void **slot = &vt[index];
	if (seen.count(slot)) return;
	for (int i = 0; i < kVariants; ++i) if (*slot == detours[i]) return; // already ours
	const int n = (int)seen.size();
	if (n >= kVariants) { seen[slot] = -1; logf("hook %s: too many variants", name); return; }
	DWORD old = 0;
	if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) { seen[slot] = -1; logf("vtable patch %s FAILED", name); return; }
	originals[n] = *slot;
	*slot = detours[n];
	VirtualProtect(slot, sizeof(void *), old, &old);
	seen[slot] = n;
	logf("vtable-patched %s variant %d (original in %s)", name, n, module_of(originals[n]).c_str());
}

// Watchdog: the patched first bytes of every command-list code hook, to detect (and log) when another component
// restores or replaces them.
struct WatchedHook { void *target; uint8_t bytes[16]; const char *name; int variant; int repairs; };
static std::vector<WatchedHook> g_watched;

static void hook_variant(void *target, void *const *detours, void **originals, std::map<void *, int> &seen, const char *name)
{
	std::lock_guard lock(g_hook_mtx);
	if (!target || seen.count(target)) return;
	const int n = (int)seen.size();
	if (n >= kVariants) { seen[target] = -1; logf("hook %s: too many variants", name); return; }
	if (MH_CreateHook(target, detours[n], &originals[n]) != MH_OK || MH_EnableHook(target) != MH_OK)
	{
		seen[target] = -1;
		logf("hook %s variant %d FAILED", name, n);
		return;
	}
	seen[target] = n;
	WatchedHook w{ target, {}, name, n, 0 };
	memcpy(w.bytes, target, sizeof(w.bytes));
	g_watched.push_back(w);
	logf("hooked %s variant %d (%s)", name, n, module_of(target).c_str());
}

template <typename T>
static bool hook_once(void *target, T *detour, T **original, const char *name)
{
	std::lock_guard lock(g_hook_mtx);
	if (*original) return true;
	if (!target || MH_CreateHook(target, reinterpret_cast<void *>(detour), reinterpret_cast<void **>(original)) != MH_OK || MH_EnableHook(target) != MH_OK)
	{
		logf("hook %s FAILED", name);
		return false;
	}
	logf("hooked %s (%s)", name, module_of(target).c_str());
	return true;
}

// Redirects one vtable slot instead of rewriting the function's code, so other tools (RTSS) can still hook the
// function itself. All objects of the class share the vtable.
static bool patch_vtable_slot(void **vt, int index, void *detour, void **original, const char *name)
{
	std::lock_guard lock(g_hook_mtx);
	if (vt[index] == detour) return true;
	DWORD old = 0;
	if (!VirtualProtect(&vt[index], sizeof(void *), PAGE_READWRITE, &old)) { logf("vtable patch %s FAILED", name); return false; }
	*original = vt[index];
	vt[index] = detour;
	VirtualProtect(&vt[index], sizeof(void *), old, &old);
	logf("vtable-patched %s", name);
	return true;
}

#define DETOUR_TABLE(name) static void *const name##_detours[kVariants] = { (void *)&name<0>, (void *)&name<1>, (void *)&name<2>, (void *)&name<3>, (void *)&name<4>, (void *)&name<5>, (void *)&name<6>, (void *)&name<7> }

// ---------------------------------------------------------------------------------------------
// Pipeline creation -> shader hashes (GI denoiser passes are recognized by compute shader hash)
// ---------------------------------------------------------------------------------------------

static ID3D12Device *g_device = nullptr;


static std::atomic<uint32_t> g_pso_counts[4]; // 0 compute desc, 1 stream, 2 library compute, 3 library stream
static void note_compute_pipeline(ID3D12RootSignature *rs, const D3D12_SHADER_BYTECODE &cs, void *pso, int path)
{
	if (!pso || !cs.pShaderBytecode || !cs.BytecodeLength) return;
	const uint32_t n = ++g_pso_counts[path];
	if (n == 1 || n % 500 == 0)
		logf("compute pipelines seen: desc=%u stream=%u lib=%u libstream=%u", g_pso_counts[0].load(), g_pso_counts[1].load(), g_pso_counts[2].load(), g_pso_counts[3].load());
	const uint64_t hash = gi::identity_hash(cs.pShaderBytecode, cs.BytecodeLength);
	if (hash == 0x2065ab87ddffd305ull || hash == 0x0f647000c451f382ull)
		logf("GI denoiser pass %016llx created via path %d (root signature %p)", (unsigned long long)hash, path, rs);
	(void)rs; (void)pso;
}

// Pipeline state streams: walk subobjects (each is alignas(void*) { type; payload; }) looking for root sig + CS.
template <typename T> struct alignas(void *) StreamSO { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type; T inner; };
static size_t stream_stride(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t)
{
	switch (t)
	{
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: return sizeof(StreamSO<ID3D12RootSignature *>);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: return sizeof(StreamSO<D3D12_SHADER_BYTECODE>);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: return sizeof(StreamSO<D3D12_STREAM_OUTPUT_DESC>);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: return sizeof(StreamSO<D3D12_BLEND_DESC>);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: return sizeof(StreamSO<UINT>);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: return sizeof(StreamSO<D3D12_RASTERIZER_DESC>);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: return sizeof(StreamSO<D3D12_DEPTH_STENCIL_DESC>);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: return sizeof(StreamSO<D3D12_INPUT_LAYOUT_DESC>);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: return sizeof(StreamSO<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: return sizeof(StreamSO<D3D12_PRIMITIVE_TOPOLOGY_TYPE>);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: return sizeof(StreamSO<D3D12_RT_FORMAT_ARRAY>);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: return sizeof(StreamSO<DXGI_FORMAT>);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: return sizeof(StreamSO<DXGI_SAMPLE_DESC>);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: return sizeof(StreamSO<UINT>);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: return sizeof(StreamSO<D3D12_CACHED_PIPELINE_STATE>);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: return sizeof(StreamSO<D3D12_PIPELINE_STATE_FLAGS>);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: return sizeof(StreamSO<D3D12_DEPTH_STENCIL_DESC1>);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: return sizeof(StreamSO<D3D12_VIEW_INSTANCING_DESC>);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2: return sizeof(StreamSO<D3D12_DEPTH_STENCIL_DESC2>);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1: return sizeof(StreamSO<D3D12_RASTERIZER_DESC1>);
	case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2: return sizeof(StreamSO<D3D12_RASTERIZER_DESC2>);
	default: return 0;
	}
}

static void note_stream(const D3D12_PIPELINE_STATE_STREAM_DESC *desc, void *pso, int path)
{
	if (!desc || !desc->pPipelineStateSubobjectStream || !pso) return;
	const uint8_t *p = static_cast<const uint8_t *>(desc->pPipelineStateSubobjectStream);
	const uint8_t *end = p + desc->SizeInBytes;
	ID3D12RootSignature *rs = nullptr; const D3D12_SHADER_BYTECODE *cs = nullptr;
	while (p + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= end)
	{
		const auto type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE *>(p);
		const size_t stride = stream_stride(type);
		if (!stride || p + stride > end)
		{
			static std::atomic<uint32_t> warned{0};
			if (warned++ < 10) logf("pipeline stream: unknown subobject type %d at offset %zu of %zu - stopped parsing", (int)type, (size_t)(p - static_cast<const uint8_t *>(desc->pPipelineStateSubobjectStream)), (size_t)desc->SizeInBytes);
			break;
		}
		if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE)
			rs = reinterpret_cast<const StreamSO<ID3D12RootSignature *> *>(p)->inner;
		else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS)
			cs = &reinterpret_cast<const StreamSO<D3D12_SHADER_BYTECODE> *>(p)->inner;
		p += stride;
	}
	if (cs) note_compute_pipeline(rs, *cs, pso, path);
}

using PFN_CreateComputePSO = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device *, const D3D12_COMPUTE_PIPELINE_STATE_DESC *, REFIID, void **);
using PFN_CreatePSO = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device2 *, const D3D12_PIPELINE_STATE_STREAM_DESC *, REFIID, void **);
using PFN_CreatePipelineLibrary = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device1 *, const void *, SIZE_T, REFIID, void **);
using PFN_LoadComputePipeline = HRESULT(STDMETHODCALLTYPE *)(ID3D12PipelineLibrary *, LPCWSTR, const D3D12_COMPUTE_PIPELINE_STATE_DESC *, REFIID, void **);
using PFN_LoadPipeline = HRESULT(STDMETHODCALLTYPE *)(ID3D12PipelineLibrary1 *, LPCWSTR, const D3D12_PIPELINE_STATE_STREAM_DESC *, REFIID, void **);
using PFN_CreateRTV = void(STDMETHODCALLTYPE *)(ID3D12Device *, ID3D12Resource *, const D3D12_RENDER_TARGET_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);

static PFN_CreateComputePSO o_CreateComputePSO = nullptr;
static PFN_CreatePSO o_CreatePSO = nullptr;
static PFN_CreatePipelineLibrary o_CreatePipelineLibrary = nullptr;
static PFN_LoadComputePipeline o_LoadComputePipeline = nullptr;
static PFN_LoadPipeline o_LoadPipeline = nullptr;
static PFN_CreateRTV o_CreateRTV = nullptr;

// GI denoiser bypass, standalone style: swap the shader of the game's own GI pipelines at creation time.
//   temporal pass -> gi_raw.hlsl (RAW_REG=t20): raw 1-ray GI -> signals A/B (u2/u3)
//   spatial pass  -> gi_copy.hlsl: copy A/B through; the game runs it twice, which lands A/B where the resolve reads
// Decided once at launch (gi_bypass && rr in fh6rr.ini, see g_swap_gi): there is no runtime state to intercept.
static constexpr uint64_t k_id_temporal = 0x2065ab87ddffd305ull, k_id_spatial = 0x0f647000c451f382ull;
#define GI_CONV(s, a) { g_gi_conv_s##s##_a##a, sizeof(g_gi_conv_s##s##_a##a) }
static const D3D12_SHADER_BYTECODE k_gi_conv[4][4] = {
	{ GI_CONV(0, 0), GI_CONV(0, 1), GI_CONV(0, 2), GI_CONV(0, 3) },
	{ GI_CONV(1, 0), GI_CONV(1, 1), GI_CONV(1, 2), GI_CONV(1, 3) },
	{ GI_CONV(2, 0), GI_CONV(2, 1), GI_CONV(2, 2), GI_CONV(2, 3) },
	{ GI_CONV(3, 0), GI_CONV(3, 1), GI_CONV(3, 2), GI_CONV(3, 3) },
};
#undef GI_CONV

static HRESULT STDMETHODCALLTYPE hk_CreateComputePSO(ID3D12Device *dev, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **out)
{
	D3D12_COMPUTE_PIPELINE_STATE_DESC swapped;
	if (g_swap_gi && desc && desc->CS.pShaderBytecode && desc->CS.BytecodeLength)
	{
		const uint64_t id = gi::identity_hash(desc->CS.pShaderBytecode, desc->CS.BytecodeLength);
		if (id == k_id_temporal || id == k_id_spatial)
		{
			swapped = *desc;
			swapped.CS = id == k_id_temporal ? k_gi_conv[g_cfg.gi_scale][g_cfg.gi_ao] : D3D12_SHADER_BYTECODE{ g_gi_copy_cs, sizeof(g_gi_copy_cs) };
			const HRESULT hr = o_CreateComputePSO(dev, &swapped, riid, out);
			logf("GI bypass: %s pass created with the replacement shader -> 0x%08lx", id == k_id_temporal ? "temporal" : "spatial", hr);
			if (SUCCEEDED(hr)) return hr;
			logf("  replacement rejected, using the game's shader (game GI denoiser stays on)"); // fall through
		}
	}
	const HRESULT hr = o_CreateComputePSO(dev, desc, riid, out);
	if (SUCCEEDED(hr) && desc && out && *out) note_compute_pipeline(desc->pRootSignature, desc->CS, *out, 0);
	return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreatePSO(ID3D12Device2 *dev, const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID riid, void **out)
{
	const HRESULT hr = o_CreatePSO(dev, desc, riid, out);
	if (SUCCEEDED(hr) && out && *out) note_stream(desc, *out, 1);
	return hr;
}

static HRESULT STDMETHODCALLTYPE hk_LoadComputePipeline(ID3D12PipelineLibrary *lib, LPCWSTR name, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **out)
{
	const HRESULT hr = o_LoadComputePipeline(lib, name, desc, riid, out);
	if (SUCCEEDED(hr) && desc && out && *out) note_compute_pipeline(desc->pRootSignature, desc->CS, *out, 2);
	return hr;
}

static HRESULT STDMETHODCALLTYPE hk_LoadPipeline(ID3D12PipelineLibrary1 *lib, LPCWSTR name, const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID riid, void **out)
{
	const HRESULT hr = o_LoadPipeline(lib, name, desc, riid, out);
	if (SUCCEEDED(hr) && out && *out) note_stream(desc, *out, 3);
	return hr;
}

static void hook_library_vtable(IUnknown *obj)
{
	{
		ID3D12PipelineLibrary *lib = nullptr;
		if (SUCCEEDED(obj->QueryInterface(IID_PPV_ARGS(&lib))) && lib)
		{
			void **vt = *reinterpret_cast<void ***>(lib);
			if (!o_LoadComputePipeline) patch_vtable_slot(vt, 10, reinterpret_cast<void *>(&hk_LoadComputePipeline), reinterpret_cast<void **>(&o_LoadComputePipeline), "ID3D12PipelineLibrary::LoadComputePipeline");
			ID3D12PipelineLibrary1 *lib1 = nullptr;
			if (SUCCEEDED(lib->QueryInterface(IID_PPV_ARGS(&lib1))) && lib1)
			{
				void **vt1 = *reinterpret_cast<void ***>(lib1);
				if (!o_LoadPipeline) patch_vtable_slot(vt1, 13, reinterpret_cast<void *>(&hk_LoadPipeline), reinterpret_cast<void **>(&o_LoadPipeline), "ID3D12PipelineLibrary1::LoadPipeline");
				lib1->Release();
			}
			lib->Release();
		}
	}
}

static HRESULT STDMETHODCALLTYPE hk_CreatePipelineLibrary(ID3D12Device1 *dev, const void *blob, SIZE_T len, REFIID riid, void **out)
{
	const HRESULT hr = o_CreatePipelineLibrary(dev, blob, len, riid, out);
	if (SUCCEEDED(hr) && out && *out) hook_library_vtable(static_cast<IUnknown *>(*out));
	return hr;
}

// RTV handle -> resource, so OMSetRenderTargets can be mapped back to the G-buffer textures
static std::shared_mutex g_rtv_mtx;
static std::unordered_map<SIZE_T, ID3D12Resource *> g_rtvs;
static UINT g_rtv_inc = 0;
static std::atomic<uint64_t> g_rtv_created{0};

static void STDMETHODCALLTYPE hk_CreateRTV(ID3D12Device *dev, ID3D12Resource *res, const D3D12_RENDER_TARGET_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
	o_CreateRTV(dev, res, desc, h);
	++g_rtv_created;
	std::unique_lock lock(g_rtv_mtx);
	g_rtvs[h.ptr] = res;
}

// ---------------------------------------------------------------------------------------------
// Command list hooks
// ---------------------------------------------------------------------------------------------

using PFN_Reset = HRESULT(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12CommandAllocator *, ID3D12PipelineState *);
using PFN_SetPSO = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12PipelineState *);
using PFN_Dispatch = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, UINT, UINT);
using PFN_OMSetRT = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE *);

static PFN_Reset o_Reset[kVariants];
static PFN_SetPSO o_SetPSO[kVariants];
static PFN_Dispatch o_Dispatch[kVariants];
static PFN_OMSetRT o_OMSetRT[kVariants];
static std::map<void *, int> g_seen_reset, g_seen_setpso, g_seen_dispatch, g_seen_omset;
static std::atomic<uint64_t> g_calls_dispatch{0}, g_calls_setpso{0}, g_calls_omset8{0};
static std::atomic<uint64_t> g_gb_ok{0}, g_gb_badfmt{0}, g_gb_missing{0};

// Expected FH6 G-buffer layout (verified with the recon tool's debug views). A bind whose 8 formats do not all
// match came from a stale RTV-map entry, and must not feed RR's guides.
static bool gbuffer_layout_ok(ID3D12Resource *const rts[8])
{
	static const DXGI_FORMAT expect[8] = {
		DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R32_UINT,
		DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_UINT };
	for (int i = 0; i < 8; ++i)
		if (!rts[i] || rts[i]->GetDesc().Format != expect[i]) return false;
	return true;
}

template <int N>
static HRESULT STDMETHODCALLTYPE hk_Reset(ID3D12GraphicsCommandList *list, ID3D12CommandAllocator *alloc, ID3D12PipelineState *pso)
{
	gi::on_reset(reinterpret_cast<uint64_t>(list));
	if (pso) gi::on_bind_compute(reinterpret_cast<uint64_t>(list), reinterpret_cast<uint64_t>(pso));
	return o_Reset[N](list, alloc, pso);
}

template <int N>
static void STDMETHODCALLTYPE hk_SetPSO(ID3D12GraphicsCommandList *list, ID3D12PipelineState *pso)
{
	++g_calls_setpso;
	gi::on_bind_compute(reinterpret_cast<uint64_t>(list), reinterpret_cast<uint64_t>(pso));
	o_SetPSO[N](list, pso);
}

template <int N>
static void STDMETHODCALLTYPE hk_Dispatch(ID3D12GraphicsCommandList *list, UINT x, UINT y, UINT z)
{
	++g_calls_dispatch;
	if (gi::enabled())
	{
		ID3D12PipelineState *swap = nullptr;
		if (gi::on_dispatch(reinterpret_cast<uint64_t>(list), &swap))
			return; // skipped (GI denoiser bypass)
		if (swap)
			list->SetPipelineState(swap); // goes through hk_SetPSO, which is fine: gi's lock is not held here
	}
	o_Dispatch[N](list, x, y, z);
}

static void hook_list_vtable(ID3D12GraphicsCommandList *list);
// Command list classes already hooked (vtable pointers). FH6 records gameplay on list classes whose Dispatch /
// SetPipelineState implementations differ from the throwaway lists' - every class in use must be hooked.
static std::atomic<void *> g_known_vtables[16];
static void ensure_list_class_hooked(ID3D12GraphicsCommandList *list)
{
	void *vt = *reinterpret_cast<void **>(list);
	for (auto &k : g_known_vtables) { void *v = k.load(std::memory_order_relaxed); if (v == vt) return; if (!v) break; }
	hook_list_vtable(list);
	for (auto &k : g_known_vtables) { void *expected = nullptr; if (k.compare_exchange_strong(expected, vt) || expected == vt) break; }
	logf("command list class %p (%s) hooked", vt, module_of(vt).c_str());
}

template <int N>
static void STDMETHODCALLTYPE hk_OMSetRT(ID3D12GraphicsCommandList *list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE *rtvs, BOOL single_range, const D3D12_CPU_DESCRIPTOR_HANDLE *dsv)
{
	// FH6 G-buffer: one 8-target MRT pass; RT3 = R32_UINT (normal+smoothness), RT4 = RGBA8 sRGB (albedo)
	// No class discovery here: D3D12Core shares this function's code with unrelated internal methods, so list is
	// not always a command list (discovery hooked NVIDIA driver functions with the wrong signatures).
	if (count == 8 && rtvs)
	{
		++g_calls_omset8;
		ID3D12Resource *rts[8] = {};
		{
			std::shared_lock lock(g_rtv_mtx);
			for (UINT i = 0; i < 8; ++i)
			{
				const SIZE_T h = single_range ? rtvs[0].ptr + SIZE_T(i) * g_rtv_inc : rtvs[i].ptr;
				auto it = g_rtvs.find(h);
				rts[i] = it != g_rtvs.end() ? it->second : nullptr;
			}
		}
		bool missing = false;
		for (auto *r : rts) if (!r) missing = true;
		if (missing) ++g_gb_missing;
		else if (rts[3]->GetDesc().Format == DXGI_FORMAT_R32_UINT && rts[4]->GetDesc().Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
		{
			if (gbuffer_layout_ok(rts)) { ++g_gb_ok; rr::on_gbuffer(rts); }
			else ++g_gb_badfmt;
		}
	}
	o_OMSetRT[N](list, count, rtvs, single_range, dsv);
}

DETOUR_TABLE(hk_Reset);
DETOUR_TABLE(hk_SetPSO);
DETOUR_TABLE(hk_Dispatch);
DETOUR_TABLE(hk_OMSetRT);

static void hook_list_vtable(ID3D12GraphicsCommandList *list)
{
	void **vt = *reinterpret_cast<void ***>(list);
	// ID3D12GraphicsCommandList vtable: 10 Reset, 14 Dispatch, 25 SetPipelineState, 46 OMSetRenderTargets
	// Code hooks (MinHook), not vtable slots: Streamline's command list wrapper calls the runtime's functions
	// through pointers it cached, which a vtable-slot redirect never sees.
	// Only the G-buffer bind hook: the GI bypass swaps shaders at creation, so no Dispatch/SetPipelineState hooks.
	(void)hk_Reset_detours; (void)hk_Dispatch_detours; (void)hk_SetPSO_detours;
	hook_variant(vt[46], hk_OMSetRT_detours, reinterpret_cast<void **>(o_OMSetRT), g_seen_omset, "OMSetRenderTargets");
}

// Create throwaway command lists of each type to reach every command list implementation's vtable.
static void hook_command_lists(ID3D12Device *dev)
{
	const D3D12_COMMAND_LIST_TYPE types[] = { D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE };
	for (const auto type : types)
	{
		ID3D12CommandAllocator *alloc = nullptr;
		ID3D12GraphicsCommandList *list = nullptr;
		if (SUCCEEDED(dev->CreateCommandAllocator(type, IID_PPV_ARGS(&alloc))) &&
			SUCCEEDED(dev->CreateCommandList(0, type, alloc, nullptr, IID_PPV_ARGS(&list))))
		{
			ensure_list_class_hooked(list);
			list->Close();
		}
		if (list) list->Release();
		if (alloc) alloc->Release();
	}
}

using PFN_CreateCommandList = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device *, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *, ID3D12PipelineState *, REFIID, void **);
using PFN_CreateCommandList1 = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device4 *, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS, REFIID, void **);
static PFN_CreateCommandList o_CreateCommandList = nullptr;
static PFN_CreateCommandList1 o_CreateCommandList1 = nullptr;

static void on_new_list(void *obj)
{
	ID3D12GraphicsCommandList *list = nullptr;
	if (obj && SUCCEEDED(static_cast<IUnknown *>(obj)->QueryInterface(IID_PPV_ARGS(&list))) && list)
	{
		ensure_list_class_hooked(list);
		list->Release();
	}
}

static HRESULT STDMETHODCALLTYPE hk_CreateCommandList(ID3D12Device *dev, UINT node, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator *alloc, ID3D12PipelineState *pso, REFIID riid, void **out)
{
	const HRESULT hr = o_CreateCommandList(dev, node, type, alloc, pso, riid, out);
	if (SUCCEEDED(hr) && out) on_new_list(*out);
	return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateCommandList1(ID3D12Device4 *dev, UINT node, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void **out)
{
	const HRESULT hr = o_CreateCommandList1(dev, node, type, flags, riid, out);
	if (SUCCEEDED(hr) && out) on_new_list(*out);
	return hr;
}

static std::once_flag g_device_hooks_once, g_list_hooks_once;
static void hook_lists(ID3D12Device *dev) { std::call_once(g_list_hooks_once, [dev]() { hook_command_lists(dev); }); }
static void hook_device(ID3D12Device *dev)
{
	std::call_once(g_device_hooks_once, [dev]() {
		void **vt = *reinterpret_cast<void ***>(dev);
		g_rtv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		// ID3D12Device vtable: 11 CreateComputePipelineState, 20 CreateRenderTargetView,
		// 47 CreatePipelineLibrary (ID3D12Device1), 50 CreatePipelineState (ID3D12Device2)
		hook_once(vt[11], &hk_CreateComputePSO, &o_CreateComputePSO, "CreateComputePipelineState");
		hook_once(vt[20], &hk_CreateRTV, &o_CreateRTV, "CreateRenderTargetView");
		// No CreateCommandList(1) hooks on Streamline's wrapper: its small forwarders can share code between methods,
		// so a hook there also catches unrelated calls (crashed). New list classes are discovered in hk_OMSetRT.
		(void)&hk_CreateCommandList; (void)&hk_CreateCommandList1;
		ID3D12Device2 *dev2 = nullptr;
		if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dev2))) && dev2)
		{
			void **vt2 = *reinterpret_cast<void ***>(dev2);
			hook_once(vt2[50], &hk_CreatePSO, &o_CreatePSO, "CreatePipelineState");
			dev2->Release();
		}
	});
}

// The game's own D3D12CreateDevice call, caught by patching forzahorizon6.exe's import table entry (the same
// technique ReShade uses). d3d12.dll itself is NOT patched: an inline hook there broke Streamline and RTSS.
// Hooking the device here, at creation, catches the pipeline cache FH6 opens before it hands the device to
// Streamline. Command list hooks are installed later (on_device_interface), which is known to work.
using PFN_D3D12CreateDevice = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
static PFN_D3D12CreateDevice o_D3D12CreateDevice = nullptr;

static HRESULT WINAPI hk_D3D12CreateDevice_iat(IUnknown *adapter, D3D_FEATURE_LEVEL level, REFIID riid, void **out)
{
	const HRESULT hr = o_D3D12CreateDevice(adapter, level, riid, out);
	if (SUCCEEDED(hr) && out && *out)
	{
		ID3D12Device *dev = nullptr;
		if (SUCCEEDED(static_cast<IUnknown *>(*out)->QueryInterface(IID_PPV_ARGS(&dev))) && dev)
		{
			g_device = dev;      // newest device; the GI module creates its replacement pipelines on it

			rr::on_device(dev);
			// Only a vtable-slot redirect this early (code hooks here broke RTSS's overlay). Everything else is
			// hooked later, when the game hands the device to Streamline.
			if (!o_CreatePipelineLibrary)
			{
				ID3D12Device1 *dev1 = nullptr;
				if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dev1))) && dev1)
				{
					patch_vtable_slot(*reinterpret_cast<void ***>(dev1), 47, reinterpret_cast<void *>(&hk_CreatePipelineLibrary), reinterpret_cast<void **>(&o_CreatePipelineLibrary), "ID3D12Device1::CreatePipelineLibrary");
					dev1->Release();
				}
			}
			logf("D3D12CreateDevice (game) -> device %p", dev);
			dev->Release();
		}
	}
	return hr;
}

static bool patch_iat(HMODULE module, const char *dll, const char *func, void *detour, void **original)
{
	auto *base = reinterpret_cast<uint8_t *>(module);
	auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
	auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
	const auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir.VirtualAddress) return false;
	HMODULE target_mod = GetModuleHandleA(dll);
	void *target = target_mod ? reinterpret_cast<void *>(GetProcAddress(target_mod, func)) : nullptr;
	for (auto *imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); imp->Name; ++imp)
	{
		if (_stricmp(reinterpret_cast<const char *>(base + imp->Name), dll) != 0) continue;
		auto *iat = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imp->FirstThunk);
		auto *names = reinterpret_cast<IMAGE_THUNK_DATA *>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
		for (; iat->u1.Function; ++iat, ++names)
		{
			bool match = target && reinterpret_cast<void *>(iat->u1.Function) == target;
			if (!match && imp->OriginalFirstThunk && !IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
				match = strcmp(reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData)->Name, func) == 0;
			if (!match) continue;
			DWORD old = 0;
			if (!VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) return false;
			*original = reinterpret_cast<void *>(iat->u1.Function);
			iat->u1.Function = reinterpret_cast<ULONG_PTR>(detour);
			VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
			return true;
		}
	}
	return false;
}

static sl::Result (*g_get_native)(void *, void **) = nullptr; // slGetNativeInterface, set once Streamline is hooked

// Called with the game's device as passed to Streamline. Newest device wins.
static void on_device_interface(void *unknown)
{
	if (!unknown) return;
	ID3D12Device *dev = nullptr;
	if (FAILED(static_cast<IUnknown *>(unknown)->QueryInterface(IID_PPV_ARGS(&dev))) || !dev) return; // e.g. a DXGI factory
	// Hook the device object Streamline hands us - normally Streamline's own wrapper - and the command lists it
	// creates. The game records everything through those wrappers, nothing else patches them, and RTSS does not
	// mind. Code hooks on the D3D12 runtime underneath were silently removed after a few frames and broke RTSS.
	ID3D12Device *target = dev;
	logf("Streamline device %p (%s, vtable in %s)", dev, is_runtime_object(dev) ? "native" : "wrapper", module_of(*reinterpret_cast<void **>(dev)).c_str());
	if (target)
	{
		if (target != g_device) { g_device = target; rr::on_device(target); }
		if (g_cfg.hook_device) hook_device(target);
		if (g_cfg.hook_lists) hook_lists(target);
	}
	dev->Release();
}
// ---------------------------------------------------------------------------------------------
// Streamline hooks (same as the add-on build)
// ---------------------------------------------------------------------------------------------

#include <sl.h>
#include <sl_dlss.h>

static PFun_slInit *o_slInit = nullptr;
static PFun_slSetTagForFrame *o_slSetTagForFrame = nullptr;
static PFun_slEvaluateFeature *o_slEvaluateFeature = nullptr;
static PFun_slSetConstants *o_slSetConstants = nullptr;
static PFun_slGetFeatureFunction *o_slGetFeatureFunction = nullptr;
static PFun_slIsFeatureLoaded *o_slIsFeatureLoaded = nullptr;
static PFun_slDLSSSetOptions *o_slDLSSSetOptions = nullptr;
static PFun_slGetNativeInterface *o_slGetNativeInterface = nullptr;
static PFun_slUpgradeInterface *o_slUpgradeInterface = nullptr;
static PFun_slSetD3DDevice *o_slSetD3DDevice = nullptr;

static sl::PFun_LogMessageCallback *g_game_sl_log = nullptr;
static void sl_log_callback(sl::LogType type, const char *msg)
{
	if (g_game_sl_log) g_game_sl_log(type, msg); // keep the game's own logging working
	if (!msg || type == sl::LogType::eInfo) return; // only warnings and errors go to fh6rr.log
	std::string s(msg);
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
	logf("SL[%u] %s", (unsigned)type, s.c_str());
}

static sl::Result hk_slUpgradeInterface(void **base)
{
	if (base && *base) on_device_interface(*base); // native interface, before Streamline wraps it
	return o_slUpgradeInterface(base);
}

static sl::Result hk_slSetD3DDevice(void *device)
{
	on_device_interface(device);
	return o_slSetD3DDevice(device);
}

static std::atomic<uint64_t> g_frame{0};
static std::atomic<uint64_t> g_rr_ok{0}, g_rr_fallback{0}, g_rr_unresolved{0};
static std::atomic<uint64_t> g_last_rr_ok_frame{0};

static sl::Result hk_slInit(const sl::Preferences &pref, uint64_t sdk_version)
{
	if (!pref.featuresToLoad || !pref.numFeaturesToLoad)
		return o_slInit(pref, sdk_version);
	static std::vector<sl::Feature> features;
	features.assign(pref.featuresToLoad, pref.featuresToLoad + pref.numFeaturesToLoad);
	if (std::find(features.begin(), features.end(), sl::kFeatureDLSS_RR) == features.end())
		features.push_back(sl::kFeatureDLSS_RR);
	sl::Preferences p = pref;
	p.featuresToLoad = features.data();
	p.numFeaturesToLoad = (uint32_t)features.size();
	// Mirror Streamline's own messages into fh6rr.log (diagnostics without ReShade in the way)
	g_game_sl_log = pref.logMessageCallback;
	p.logMessageCallback = sl_log_callback;
	logf("slInit: game flags=0x%llx logLevel=%u callback=%p plugin paths=%u", (unsigned long long)pref.flags, (unsigned)pref.logLevel, pref.logMessageCallback, pref.numPathsToPlugins);
	const sl::Result r = o_slInit(p, sdk_version);
	logf("slInit: added DLSS Ray Reconstruction to %u game features -> %d", pref.numFeaturesToLoad, (int)r);
	return r;
}

static sl::Result hk_slDLSSSetOptions(const sl::ViewportHandle &vp, const sl::DLSSOptions &o)
{
	rr::on_dlss_options(o);
	return o_slDLSSSetOptions(vp, o);
}

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

// The list the game hands Streamline may be wrapped by Streamline; unwrap to the D3D12 runtime's object.
static ID3D12GraphicsCommandList *resolve_native_list(void *cmd)
{
	if (!cmd) return nullptr;
	static HMODULE d3d12core = GetModuleHandleW(L"D3D12Core.dll");
	if (d3d12core && is_runtime_object(cmd)) return static_cast<ID3D12GraphicsCommandList *>(cmd);
	if (o_slGetNativeInterface)
	{
		void *base = nullptr;
		if (o_slGetNativeInterface(cmd, &base) == sl::Result::eOk && base && (!d3d12core || is_runtime_object(base)))
			return static_cast<ID3D12GraphicsCommandList *>(base);
	}
	return nullptr;
}

static std::string hex16(const uint8_t *b) { char s[64]; for (int i = 0; i < 16; ++i) snprintf(s + i * 3, 4, "%02X ", b[i]); return s; }

static void check_hooks()
{
	std::lock_guard lock(g_hook_mtx);
	for (WatchedHook &w : g_watched)
	{
		const uint8_t *now = static_cast<const uint8_t *>(w.target);
		if (memcmp(now, w.bytes, sizeof(w.bytes)) == 0) continue;
		std::string where = "-";
		if (now[0] == 0xE9) { int32_t rel; memcpy(&rel, now + 1, 4); where = module_of(now + 5 + rel); }
		else if (now[0] == 0xFF && now[1] == 0x25) { int32_t rel; memcpy(&rel, now + 2, 4); void *dst; memcpy(&dst, now + 6 + rel, 8); where = module_of(dst); }
		logf("hook %s variant %d was overwritten: now [%s] (jump into %s), ours was [%s]", w.name, w.variant, hex16(now).c_str(), where.c_str(), hex16(w.bytes).c_str());
		if (w.repairs++ < 3)
		{
			DWORD old = 0;
			if (VirtualProtect(w.target, sizeof(w.bytes), PAGE_EXECUTE_READWRITE, &old))
			{
				memcpy(w.target, w.bytes, sizeof(w.bytes));
				VirtualProtect(w.target, sizeof(w.bytes), old, &old);
				FlushInstructionCache(GetCurrentProcess(), w.target, sizeof(w.bytes));
				logf("  re-applied %s (repair %d)", w.name, w.repairs);
			}
		}
	}
}

// Once per frame (the game's DLSS evaluate): frame counter and periodic diagnostics. No hotkeys: the GI bypass is
// applied at pipeline creation, so settings only change with a restart.
static void per_frame()
{
	const uint64_t frame = ++g_frame;
	if (frame % 300 == 0) check_hooks();
	if (frame == 120 || frame % 3600 == 0)
		logf("frame %llu: RR ok %llu, fallback %llu, lists not unwrappable %llu; G-buffer binds ok %llu bad %llu missing %llu", (unsigned long long)frame, (unsigned long long)g_rr_ok.load(), (unsigned long long)g_rr_fallback.load(), (unsigned long long)g_rr_unresolved.load(), (unsigned long long)g_gb_ok.load(), (unsigned long long)g_gb_badfmt.load(), (unsigned long long)g_gb_missing.load());
	gi::set_allowed(rr::enabled() && frame - g_last_rr_ok_frame.load() <= 2);
}

static sl::Result hk_slEvaluateFeature(sl::Feature feature, const sl::FrameToken &frame, const sl::BaseStructure **inputs, uint32_t num, sl::CommandBuffer *cmd)
{
	if (feature == sl::kFeatureDLSS)
	{
		per_frame();
		if (rr::enabled())
		{
			ID3D12GraphicsCommandList *native = resolve_native_list(cmd);
			if (!native && cmd)
			{
				// Not unwrappable (e.g. a list whose vtable was swapped by another layer): it is still a working
				// ID3D12GraphicsCommandList, so record the guide pass on it directly instead of giving up on RR.
				if (g_rr_unresolved++ == 0)
					logf("RR: command list %p not unwrappable (vtable in %s) - recording on it directly", cmd, module_of(*reinterpret_cast<void **>(cmd)).c_str());
				native = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd);
			}
			if (rr::evaluate(frame, inputs, num, cmd, native))
			{
				++g_rr_ok;
				g_last_rr_ok_frame = g_frame.load();
				return sl::Result::eOk; // RR replaced DLSS-SR this frame
			}
			if (g_rr_fallback++ == 0) logf("RR: first fallback to DLSS-SR at frame %llu", (unsigned long long)g_frame.load());
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
static std::mutex g_sl_mtx;

static void try_hook_streamline()
{
	if (g_sl_hooked.load()) return;
	std::lock_guard lock(g_sl_mtx);
	if (g_sl_hooked.load()) return;
	HMODULE sl = GetModuleHandleW(L"sl.interposer.dll");
	if (!sl) return;
	bool ok = hook_export(sl, "slInit", &hk_slInit, &o_slInit);
	ok &= hook_export(sl, "slEvaluateFeature", &hk_slEvaluateFeature, &o_slEvaluateFeature);
	ok &= hook_export(sl, "slSetConstants", &hk_slSetConstants, &o_slSetConstants);
	ok &= hook_export(sl, "slGetFeatureFunction", &hk_slGetFeatureFunction, &o_slGetFeatureFunction);
	ok &= hook_export(sl, "slUpgradeInterface", &hk_slUpgradeInterface, &o_slUpgradeInterface);
	ok &= hook_export(sl, "slSetD3DDevice", &hk_slSetD3DDevice, &o_slSetD3DDevice);
	o_slSetTagForFrame = reinterpret_cast<PFun_slSetTagForFrame *>(GetProcAddress(sl, "slSetTagForFrame"));
	o_slIsFeatureLoaded = reinterpret_cast<PFun_slIsFeatureLoaded *>(GetProcAddress(sl, "slIsFeatureLoaded"));
	o_slGetNativeInterface = reinterpret_cast<PFun_slGetNativeInterface *>(GetProcAddress(sl, "slGetNativeInterface"));
	g_get_native = reinterpret_cast<sl::Result (*)(void *, void **)>(o_slGetNativeInterface);
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
	if (name && (wcsstr(name, L"sl.") || wcsstr(name, L"nvngx") || wcsstr(name, L"NVNGX")))
	{
		const DWORD err = m ? 0 : GetLastError();
		char n8[MAX_PATH] = {};
		WideCharToMultiByte(CP_UTF8, 0, name, -1, n8, MAX_PATH - 1, nullptr, nullptr);
		logf("LoadLibraryExW(%s, flags=0x%lx) -> %p err=%lu", n8, flags, m, err);
		SetLastError(err);
	}
	if (m && name && !g_sl_hooked.load() && wcsstr(name, L"sl.interposer"))
		try_hook_streamline();
	return m;
}

// ---------------------------------------------------------------------------------------------
// Startup (called from the proxy's DllMain)
// ---------------------------------------------------------------------------------------------

void fh6rr_standalone_init(HMODULE self)
{
	wchar_t path[MAX_PATH] = {};
	GetModuleFileNameW(self, path, MAX_PATH);
	g_dir = std::filesystem::path(path).parent_path();
	g_log = _wfopen((g_dir / "fh6rr.log").c_str(), L"w");
	logf("FH6 DLSS Ray Reconstruction (standalone) loaded");
	load_config();
	rr::set_enabled(g_cfg.rr);
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
	// No D3D12CreateDevice interception: every build that intercepted it (inline or via the import table) broke
	// RTSS's overlay, and FH6 does not use a pipeline library, which was the only reason for it.
	(void)&patch_iat; (void)&hk_D3D12CreateDevice_iat;
	try_hook_streamline();
}

void fh6rr_standalone_shutdown()
{
	MH_DisableHook(MH_ALL_HOOKS);
	MH_Uninitialize();
	if (g_log) { fclose(g_log); g_log = nullptr; }
}
