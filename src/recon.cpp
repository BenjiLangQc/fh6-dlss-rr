// FH6 RR - Phase 1 recon addon (ReShade 6.x, D3D12)
//
// Observes, does not modify, the game's rendering:
//   * Streamline calls (tags, constants, DLSS options, evaluate) -> fh6rr_recon.log
//   * Press F9 in-game to dump one full frame: every compute dispatch / ray dispatch /
//     draw batch in submission order, with PIX marker names and bound textures,
//     plus the compute shader bytecode of every pass -> fh6rr_capture_N.txt, fh6rr_shaders/
//
// Nothing here changes what the game renders.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>

#include <reshade.hpp>
#include "descriptor_tracking.hpp"
#include "state_tracking.hpp"

#include <MinHook.h>
#include "debug_view.h"
#include "rr.h"
#include "gi_bypass.h"

#include <sl.h>
#include <sl_dlss.h>
#include <sl_dlss_d.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace reshade::api;

extern "C" __declspec(dllexport) const char *NAME = "FH6 RR Recon";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Phase 1: logs Streamline/DLSS inputs and dumps a frame's compute passes (F9). Read-only.";

// ---------------------------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------------------------

struct Config
{
	bool streamline = true;   // Streamline export hooks
	bool markers = true;      // PIX marker hooks on the command list vtable
	bool tracking = true;     // descriptor + state tracking (needed for bindings in captures)
	bool pipelines = true;    // pipeline hashing / bytecode copies
	bool loadlibrary = true;  // LoadLibraryExW hook
	bool commands = true;     // dispatch/draw/execute events
	bool descriptors = true;  // native descriptor/root-signature tracking for bindings
};
static Config g_cfg;

static std::string addr_str(DWORD64 addr);
static HMODULE g_module = nullptr;
static std::filesystem::path g_dir;
static std::mutex g_log_mtx;
static FILE *g_log = nullptr;

void logf(const char *fmt, ...)
{
	std::lock_guard lock(g_log_mtx);
	if (!g_log)
		return;
	SYSTEMTIME st; GetLocalTime(&st);
	fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	va_list args; va_start(args, fmt);
	vfprintf(g_log, fmt, args);
	va_end(args);
	fputc('\n', g_log);
	fflush(g_log);
}

static std::string strf(const char *fmt, ...)
{
	char buf[2048];
	va_list args; va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	return buf;
}

static uint64_t fnv1a(const void *data, size_t size)
{
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < size; ++i) { h ^= static_cast<const uint8_t *>(data)[i]; h *= 1099511628211ull; }
	return h;
}

// ---------------------------------------------------------------------------------------------
// D3D12 resource description helpers
// ---------------------------------------------------------------------------------------------

static const char *dxgi_name(DXGI_FORMAT f)
{
	switch (f)
	{
	case DXGI_FORMAT_UNKNOWN: return "UNKNOWN";
	case DXGI_FORMAT_R32G32B32A32_FLOAT: return "RGBA32F";
	case DXGI_FORMAT_R32G32B32A32_UINT: return "RGBA32U";
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "RGBA16_TL";
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
	case DXGI_FORMAT_R16G16B16A16_UNORM: return "RGBA16UN";
	case DXGI_FORMAT_R16G16B16A16_UINT: return "RGBA16U";
	case DXGI_FORMAT_R16G16B16A16_SNORM: return "RGBA16SN";
	case DXGI_FORMAT_R32G32_FLOAT: return "RG32F";
	case DXGI_FORMAT_R32G32_UINT: return "RG32U";
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: return "RGB10A2_TL";
	case DXGI_FORMAT_R10G10B10A2_UNORM: return "RGB10A2UN";
	case DXGI_FORMAT_R10G10B10A2_UINT: return "RGB10A2U";
	case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10F";
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "RGBA8_TL";
	case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8UN";
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "RGBA8_SRGB";
	case DXGI_FORMAT_R8G8B8A8_UINT: return "RGBA8U";
	case DXGI_FORMAT_R8G8B8A8_SNORM: return "RGBA8SN";
	case DXGI_FORMAT_R16G16_TYPELESS: return "RG16_TL";
	case DXGI_FORMAT_R16G16_FLOAT: return "RG16F";
	case DXGI_FORMAT_R16G16_UNORM: return "RG16UN";
	case DXGI_FORMAT_R16G16_UINT: return "RG16U";
	case DXGI_FORMAT_R16G16_SNORM: return "RG16SN";
	case DXGI_FORMAT_R32_TYPELESS: return "R32_TL";
	case DXGI_FORMAT_D32_FLOAT: return "D32F";
	case DXGI_FORMAT_R32_FLOAT: return "R32F";
	case DXGI_FORMAT_R32_UINT: return "R32U";
	case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TL";
	case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
	case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TL";
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32FS8";
	case DXGI_FORMAT_R8G8_UNORM: return "RG8UN";
	case DXGI_FORMAT_R8G8_UINT: return "RG8U";
	case DXGI_FORMAT_R8G8_SNORM: return "RG8SN";
	case DXGI_FORMAT_R16_TYPELESS: return "R16_TL";
	case DXGI_FORMAT_R16_FLOAT: return "R16F";
	case DXGI_FORMAT_D16_UNORM: return "D16";
	case DXGI_FORMAT_R16_UNORM: return "R16UN";
	case DXGI_FORMAT_R16_UINT: return "R16U";
	case DXGI_FORMAT_R8_UNORM: return "R8UN";
	case DXGI_FORMAT_R8_UINT: return "R8U";
	case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: return "RGB9E5";
	case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8UN";
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "BGRA8_SRGB";
	case DXGI_FORMAT_BC1_UNORM: return "BC1";
	case DXGI_FORMAT_BC3_UNORM: return "BC3";
	case DXGI_FORMAT_BC5_UNORM: return "BC5";
	case DXGI_FORMAT_BC7_UNORM: return "BC7";
	default: return nullptr;
	}
}

static std::string format_str(DXGI_FORMAT f)
{
	const char *n = dxgi_name(f);
	return n ? std::string(n) : strf("fmt%u", (unsigned)f);
}

static std::string resource_name(ID3D12Resource *r)
{
	if (!r) return {};
	wchar_t wbuf[256] = {};
	UINT size = sizeof(wbuf) - sizeof(wchar_t);
	if (SUCCEEDED(r->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, wbuf)) && size)
	{
		char out[256] = {};
		WideCharToMultiByte(CP_UTF8, 0, wbuf, size / sizeof(wchar_t), out, sizeof(out) - 1, nullptr, nullptr);
		return out;
	}
	char abuf[256] = {};
	size = sizeof(abuf) - 1;
	if (SUCCEEDED(r->GetPrivateData(WKPDID_D3DDebugObjectName, &size, abuf)) && size)
		return std::string(abuf, strnlen(abuf, size));
	return {};
}

static std::string describe_native(ID3D12Resource *r)
{
	if (!r) return "null";
	const D3D12_RESOURCE_DESC d = r->GetDesc();
	std::string s;
	if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
		s = strf("buffer %llu bytes", (unsigned long long)d.Width);
	else
		s = strf("%llux%u%s %s mips=%u", (unsigned long long)d.Width, d.Height,
			d.DepthOrArraySize > 1 ? strf("x%u", d.DepthOrArraySize).c_str() : "",
			format_str(d.Format).c_str(), d.MipLevels);
	if (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) s += " RT";
	if (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) s += " DS";
	if (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) s += " UAV";
	const std::string name = resource_name(r);
	if (!name.empty()) s += " \"" + name + "\"";
	return s;
}

// Resource registry used by captures (stable short ids R0, R1, ...)
static std::mutex g_res_mtx;
static std::unordered_map<uint64_t, int> g_res_ids;
static std::vector<std::pair<uint64_t, std::string>> g_res_list;

static int res_id(uint64_t native)
{
	if (!native) return -1;
	std::lock_guard lock(g_res_mtx);
	auto it = g_res_ids.find(native);
	if (it != g_res_ids.end()) return it->second;
	const int id = (int)g_res_list.size();
	g_res_ids.emplace(native, id);
	g_res_list.emplace_back(native, describe_native(reinterpret_cast<ID3D12Resource *>(native)));
	return id;
}

// ---------------------------------------------------------------------------------------------
// Capture state
// ---------------------------------------------------------------------------------------------

enum class EntryKind { dispatch, dispatch_indirect, rays, rays_indirect, draws, rt_bind, marker_begin, marker_end, marker, note };

struct Binding
{
	uint16_t param, reg, space;
	bool uav;
	int res; // registry id
};

struct Entry
{
	EntryKind kind;
	uint64_t pipe_hash = 0;
	uint32_t x = 0, y = 0, z = 0;
	uint32_t count = 0;
	std::string text;
	std::vector<Binding> binds;
	std::vector<int> rts; // render targets, last = depth (or -1)
};

struct ListRecord
{
	std::vector<Entry> entries;
};

static std::atomic<int> g_capture_state{0}; // 0 idle, 1 warmup, 2 recording frame
static std::atomic<bool> g_recording{false};
static std::mutex g_rec_mtx;
static std::unordered_map<uint64_t, ListRecord> g_lists; // keyed by native command list
struct Executed { uint64_t queue, list; ListRecord rec; };
static std::vector<Executed> g_frame;
static int g_capture_index = 0;
static uint64_t g_frame_counter = 0;

thread_local uint64_t t_last_list = 0;

static void push_entry(uint64_t list, Entry &&e)
{
	std::lock_guard lock(g_rec_mtx);
	auto &rec = g_lists[list];
	if (e.kind == EntryKind::draws && !rec.entries.empty() && rec.entries.back().kind == EntryKind::draws && rec.entries.back().pipe_hash == e.pipe_hash)
	{
		rec.entries.back().count += e.count;
		return;
	}
	rec.entries.push_back(std::move(e));
}

// ---------------------------------------------------------------------------------------------
// Pipelines
// ---------------------------------------------------------------------------------------------

struct PipeInfo
{
	char type = '?'; // C = compute, G = graphics, R = ray tracing
	uint64_t hash = 0;
	std::vector<uint8_t> code; // compute / ray tracing library bytecode
};

static std::shared_mutex g_pipe_mtx;
static std::unordered_map<uint64_t, PipeInfo> g_pipes;
// Shader bytecode deduplicated by hash (many PSOs share one pixel shader)
static std::mutex g_code_mtx;
static std::unordered_map<uint64_t, std::pair<char, std::vector<uint8_t>>> g_shader_code;

static void on_init_pipeline(device *dev, pipeline_layout layout, uint32_t count, const pipeline_subobject *subobjects, pipeline pipe)
{
	PipeInfo info;
	for (uint32_t i = 0; i < count; ++i)
	{
		const auto &so = subobjects[i];
		if (!so.data || !so.count) continue;
		switch (so.type)
		{
		case pipeline_subobject_type::compute_shader:
		{
			const auto *sd = static_cast<const shader_desc *>(so.data);
			if (sd->code && sd->code_size)
			{
				info.type = 'C';
				info.hash = fnv1a(sd->code, sd->code_size);
				info.code.assign(static_cast<const uint8_t *>(sd->code), static_cast<const uint8_t *>(sd->code) + sd->code_size);
			}
			break;
		}
		case pipeline_subobject_type::pixel_shader:
		{
			const auto *sd = static_cast<const shader_desc *>(so.data);
			if (sd->code && sd->code_size && info.type != 'C')
			{
				info.type = 'G';
				info.hash = fnv1a(sd->code, sd->code_size);
				info.code.assign(static_cast<const uint8_t *>(sd->code), static_cast<const uint8_t *>(sd->code) + sd->code_size);
			}
			break;
		}
		case pipeline_subobject_type::raygen_shader:
		case pipeline_subobject_type::miss_shader:
		case pipeline_subobject_type::closest_hit_shader:
		case pipeline_subobject_type::any_hit_shader:
		{
			const auto *sd = static_cast<const shader_desc *>(so.data);
			info.type = 'R';
			for (uint32_t k = 0; k < so.count; ++k)
			{
				if (!sd[k].code || !sd[k].code_size) continue;
				info.hash ^= fnv1a(sd[k].code, sd[k].code_size) + k;
				if (info.code.empty())
					info.code.assign(static_cast<const uint8_t *>(sd[k].code), static_cast<const uint8_t *>(sd[k].code) + sd[k].code_size);
			}
			break;
		}
		default:
			break;
		}
	}
	if (info.type == '?') return;
	if (info.type == 'C')
	{
		const auto *cs = [&]() -> const shader_desc * { for (uint32_t i = 0; i < count; ++i) if (subobjects[i].type == pipeline_subobject_type::compute_shader) return static_cast<const shader_desc *>(subobjects[i].data); return nullptr; }();
		if (cs && cs->code) gi::on_pipeline(reinterpret_cast<ID3D12Device *>(dev->get_native()), reinterpret_cast<ID3D12RootSignature *>(layout.handle), gi::identity_hash(cs->code, cs->code_size), pipe.handle);
	}
	if (!info.code.empty())
	{
		static size_t total_bytes = 0;
		std::lock_guard code_lock(g_code_mtx);
		if (total_bytes < (384ull << 20) && !g_shader_code.count(info.hash)) // cap memory used for bytecode copies
		{
			total_bytes += info.code.size();
			g_shader_code.try_emplace(info.hash, info.type, std::move(info.code));
		}
		info.code.clear(); info.code.shrink_to_fit();
	}
	std::unique_lock lock(g_pipe_mtx);
	g_pipes[pipe.handle] = std::move(info);
}

static void on_destroy_pipeline(device *, pipeline pipe)
{
	gi::on_destroy_pipeline(pipe.handle);
	std::unique_lock lock(g_pipe_mtx);
	g_pipes.erase(pipe.handle);
}

static uint64_t pipe_hash(pipeline p)
{
	std::shared_lock lock(g_pipe_mtx);
	auto it = g_pipes.find(p.handle);
	return it != g_pipes.end() ? it->second.hash : 0;
}

// ---------------------------------------------------------------------------------------------
// Native descriptor tracking (lightweight replacement for ReShade's descriptor_tracking, which
// crashed FH6 ~20 s into gameplay). Hooks ID3D12Device view creation / descriptor copies and keeps
// one 64-bit slot (resource pointer | uav bit) per CBV/SRV/UAV descriptor.
// ---------------------------------------------------------------------------------------------

struct HeapInfo
{
	uint64_t cpu_start = 0, gpu_start = 0;
	uint32_t count = 0, inc = 0;
	std::unique_ptr<std::atomic<uint64_t>[]> slots;
};

static std::shared_mutex g_heap_mtx;
static std::map<uint64_t, std::shared_ptr<HeapInfo>> g_heaps_cpu;
static std::map<uint64_t, std::shared_ptr<HeapInfo>> g_heaps_gpu;
static std::atomic<uint64_t> g_heap_count{0}, g_view_count{0}, g_copy_count{0};

static HeapInfo *find_heap(const std::map<uint64_t, std::shared_ptr<HeapInfo>> &m, uint64_t ptr, uint64_t HeapInfo::*start, uint32_t &index)
{
	if (m.empty()) return nullptr;
	auto it = m.upper_bound(ptr);
	if (it == m.begin()) return nullptr;
	--it;
	HeapInfo *h = it->second.get();
	const uint64_t base = h->*start;
	if (ptr >= base + uint64_t(h->count) * h->inc) return nullptr;
	index = uint32_t((ptr - base) / h->inc);
	return h;
}

struct RootRange { D3D12_DESCRIPTOR_RANGE_TYPE type; uint32_t count, base_reg, space, offset; };
struct RootParam { D3D12_ROOT_PARAMETER_TYPE type; std::vector<RootRange> ranges; };
struct RootSigInfo { uint32_t flags = 0; std::vector<RootParam> params; };
static std::shared_mutex g_rs_mtx;
static std::unordered_map<uint64_t, RootSigInfo> g_rootsigs;

struct ListBindState { uint64_t rootsig = 0; uint64_t tables[64] = {}; uint64_t compute_pipe = 0, rt_pipe = 0, gfx_pipe = 0; uint64_t gfx_rootsig = 0; uint64_t gfx_tables[64] = {}; };
static std::mutex g_lbs_mtx;
static std::unordered_map<uint64_t, ListBindState> g_list_binds;

using PFN_CreateDescriptorHeap = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device *, const D3D12_DESCRIPTOR_HEAP_DESC *, REFIID, void **);
using PFN_CreateRootSignature = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device *, UINT, const void *, SIZE_T, REFIID, void **);
using PFN_CreateCBV = void(STDMETHODCALLTYPE *)(ID3D12Device *, const D3D12_CONSTANT_BUFFER_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CreateSRV = void(STDMETHODCALLTYPE *)(ID3D12Device *, ID3D12Resource *, const D3D12_SHADER_RESOURCE_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CreateUAV = void(STDMETHODCALLTYPE *)(ID3D12Device *, ID3D12Resource *, ID3D12Resource *, const D3D12_UNORDERED_ACCESS_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CopyDescriptors = void(STDMETHODCALLTYPE *)(ID3D12Device *, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *, const UINT *, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *, const UINT *, D3D12_DESCRIPTOR_HEAP_TYPE);
using PFN_CopyDescriptorsSimple = void(STDMETHODCALLTYPE *)(ID3D12Device *, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
using PFN_SetComputeRootSignature = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12RootSignature *);
using PFN_SetComputeRootDescriptorTable = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);

static PFN_CreateDescriptorHeap o_CreateDescriptorHeap = nullptr;
static PFN_CreateRootSignature o_CreateRootSignature = nullptr;
static PFN_CreateCBV o_CreateCBV = nullptr;
static PFN_CreateSRV o_CreateSRV = nullptr;
static PFN_CreateUAV o_CreateUAV = nullptr;
static PFN_CopyDescriptors o_CopyDescriptors = nullptr;
static PFN_CopyDescriptorsSimple o_CopyDescriptorsSimple = nullptr;

static HRESULT STDMETHODCALLTYPE hk_CreateDescriptorHeap(ID3D12Device *self, const D3D12_DESCRIPTOR_HEAP_DESC *desc, REFIID riid, void **out)
{
	const HRESULT hr = o_CreateDescriptorHeap(self, desc, riid, out);
	if (SUCCEEDED(hr) && out && *out && desc && desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && desc->NumDescriptors)
	{
		ID3D12DescriptorHeap *heap = nullptr;
		if (SUCCEEDED(static_cast<IUnknown *>(*out)->QueryInterface(IID_PPV_ARGS(&heap))) && heap)
		{
			auto info = std::make_shared<HeapInfo>();
			info->cpu_start = heap->GetCPUDescriptorHandleForHeapStart().ptr;
			info->gpu_start = (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) ? heap->GetGPUDescriptorHandleForHeapStart().ptr : 0;
			info->count = desc->NumDescriptors;
			info->inc = self->GetDescriptorHandleIncrementSize(desc->Type);
			info->slots.reset(new std::atomic<uint64_t>[desc->NumDescriptors]());
			heap->Release();
			std::unique_lock lock(g_heap_mtx);
			g_heaps_cpu[info->cpu_start] = info;
			if (info->gpu_start) g_heaps_gpu[info->gpu_start] = info;
			++g_heap_count;
			if (desc->NumDescriptors >= 100000 || info->gpu_start)
				logf("descriptor heap: %u descriptors%s", desc->NumDescriptors, info->gpu_start ? " (shader visible)" : "");
		}
	}
	return hr;
}

static void set_slot(uint64_t cpu, uint64_t value)
{
	std::shared_lock lock(g_heap_mtx);
	uint32_t idx = 0;
	if (HeapInfo *h = find_heap(g_heaps_cpu, cpu, &HeapInfo::cpu_start, idx))
		h->slots[idx].store(value, std::memory_order_relaxed);
}

static void STDMETHODCALLTYPE hk_CreateCBV(ID3D12Device *self, const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
	o_CreateCBV(self, desc, dest);
	set_slot(dest.ptr, 0);
}
static void STDMETHODCALLTYPE hk_CreateSRV(ID3D12Device *self, ID3D12Resource *res, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
	o_CreateSRV(self, res, desc, dest);
	set_slot(dest.ptr, reinterpret_cast<uint64_t>(res));
	++g_view_count;
}
static void STDMETHODCALLTYPE hk_CreateUAV(ID3D12Device *self, ID3D12Resource *res, ID3D12Resource *counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
	o_CreateUAV(self, res, counter, desc, dest);
	set_slot(dest.ptr, res ? (reinterpret_cast<uint64_t>(res) | 1) : 0);
	++g_view_count;
}

static void copy_slots(uint64_t dst, uint64_t src, uint32_t n)
{
	std::shared_lock lock(g_heap_mtx);
	uint32_t di = 0, si = 0;
	HeapInfo *dh = find_heap(g_heaps_cpu, dst, &HeapInfo::cpu_start, di);
	if (!dh) return;
	HeapInfo *sh = find_heap(g_heaps_cpu, src, &HeapInfo::cpu_start, si);
	n = std::min(n, dh->count - di);
	if (sh) n = std::min(n, sh->count - si);
	for (uint32_t i = 0; i < n; ++i)
		dh->slots[di + i].store(sh ? sh->slots[si + i].load(std::memory_order_relaxed) : 0, std::memory_order_relaxed);
}

static void STDMETHODCALLTYPE hk_CopyDescriptorsSimple(ID3D12Device *self, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
	o_CopyDescriptorsSimple(self, n, dst, src, type);
	if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) { copy_slots(dst.ptr, src.ptr, n); ++g_copy_count; }
}

static void STDMETHODCALLTYPE hk_CopyDescriptors(ID3D12Device *self, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE *ds, const UINT *dsz, UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE *ss, const UINT *ssz, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
	o_CopyDescriptors(self, nd, ds, dsz, ns, ss, ssz, type);
	if (type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !ds || !ss) return;
	++g_copy_count;
	// Walk destination and source range lists in lock step
	UINT di = 0, si = 0, doff = 0, soff = 0;
	while (di < nd && si < ns)
	{
		const UINT dlen = dsz ? dsz[di] : 1, slen = ssz ? ssz[si] : 1;
		const UINT n = std::min(dlen - doff, slen - soff);
		const uint64_t dptr = ds[di].ptr, sptr = ss[si].ptr;
		uint32_t dinc = 32;
		{
			std::shared_lock lock(g_heap_mtx); uint32_t idx;
			if (HeapInfo *h = find_heap(g_heaps_cpu, dptr, &HeapInfo::cpu_start, idx)) dinc = h->inc;
		}
		copy_slots(dptr + uint64_t(doff) * dinc, sptr + uint64_t(soff) * dinc, n);
		doff += n; soff += n;
		if (doff >= dlen) { ++di; doff = 0; }
		if (soff >= slen) { ++si; soff = 0; }
	}
}

static HRESULT STDMETHODCALLTYPE hk_CreateRootSignature(ID3D12Device *self, UINT node, const void *blob, SIZE_T len, REFIID riid, void **out)
{
	const HRESULT hr = o_CreateRootSignature(self, node, blob, len, riid, out);
	if (FAILED(hr) || !out || !*out) return hr;

	using PFN_Deser = HRESULT(WINAPI *)(LPCVOID, SIZE_T, REFIID, void **);
	static PFN_Deser deser_fn = reinterpret_cast<PFN_Deser>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12CreateVersionedRootSignatureDeserializer"));
	if (!deser_fn) return hr;
	ID3D12VersionedRootSignatureDeserializer *deser = nullptr;
	if (FAILED(deser_fn(blob, len, IID_PPV_ARGS(&deser))) || !deser) return hr;
	const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *vd = nullptr;
	if (SUCCEEDED(deser->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &vd)) && vd)
	{
		const D3D12_ROOT_SIGNATURE_DESC1 &d = vd->Desc_1_1;
		RootSigInfo info; info.flags = d.Flags;
		for (UINT p = 0; p < d.NumParameters; ++p)
		{
			RootParam rp; rp.type = d.pParameters[p].ParameterType;
			if (rp.type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
			{
				const auto &t = d.pParameters[p].DescriptorTable;
				uint32_t running = 0;
				for (UINT r = 0; r < t.NumDescriptorRanges; ++r)
				{
					const auto &range = t.pDescriptorRanges[r];
					const uint32_t off = range.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND ? running : range.OffsetInDescriptorsFromTableStart;
					rp.ranges.push_back({ range.RangeType, range.NumDescriptors, range.BaseShaderRegister, range.RegisterSpace, off });
					running = range.NumDescriptors == UINT_MAX ? off : off + range.NumDescriptors;
				}
			}
			info.params.push_back(std::move(rp));
		}
		std::unique_lock lock(g_rs_mtx);
		g_rootsigs[reinterpret_cast<uint64_t>(*out)] = std::move(info);
	}
	deser->Release();
	return hr;
}

// Command list methods can have a different implementation per list type, so every distinct
// function address is hooked separately (one templated detour + trampoline per variant).
constexpr int kVariants = 8;
static PFN_SetComputeRootSignature o_SCRS[kVariants];
static PFN_SetComputeRootDescriptorTable o_SCRDT[kVariants];
static std::atomic<uint64_t> g_scrs_calls{0}, g_scrdt_calls{0}, g_marker_calls{0};

template <int N>
static void STDMETHODCALLTYPE hk_SCRS(ID3D12GraphicsCommandList *self, ID3D12RootSignature *rs)
{
	if (g_recording.load(std::memory_order_relaxed))
	{
		++g_scrs_calls;
		std::lock_guard lock(g_lbs_mtx);
		auto &s = g_list_binds[reinterpret_cast<uint64_t>(self)];
		s.rootsig = reinterpret_cast<uint64_t>(rs);
		memset(s.tables, 0, sizeof(s.tables));
	}
	o_SCRS[N](self, rs);
}

template <int N>
static void STDMETHODCALLTYPE hk_SCRDT(ID3D12GraphicsCommandList *self, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE gpu)
{
	if (g_recording.load(std::memory_order_relaxed) && index < 64)
	{
		++g_scrdt_calls;
		std::lock_guard lock(g_lbs_mtx);
		g_list_binds[reinterpret_cast<uint64_t>(self)].tables[index] = gpu.ptr;
	}
	o_SCRDT[N](self, index, gpu);
}

static void *const g_scrs_detours[kVariants] = { (void *)&hk_SCRS<0>, (void *)&hk_SCRS<1>, (void *)&hk_SCRS<2>, (void *)&hk_SCRS<3>, (void *)&hk_SCRS<4>, (void *)&hk_SCRS<5>, (void *)&hk_SCRS<6>, (void *)&hk_SCRS<7> };
static void *const g_scrdt_detours[kVariants] = { (void *)&hk_SCRDT<0>, (void *)&hk_SCRDT<1>, (void *)&hk_SCRDT<2>, (void *)&hk_SCRDT<3>, (void *)&hk_SCRDT<4>, (void *)&hk_SCRDT<5>, (void *)&hk_SCRDT<6>, (void *)&hk_SCRDT<7> };

static std::mutex g_vhook_mtx;
static void hook_variant(void *target, void *const *detours, void **originals, std::map<void *, int> &seen, const char *name)
{
	std::lock_guard lock(g_vhook_mtx);
	if (!target || seen.count(target)) return;
	const int n = (int)seen.size();
	if (n >= kVariants) { seen[target] = -1; logf("hook %s: too many variants, %p skipped", name, target); return; }
	if (MH_CreateHook(target, detours[n], &originals[n]) != MH_OK || MH_EnableHook(target) != MH_OK)
	{
		seen[target] = -1;
		logf("hook %s variant %d at %p FAILED", name, n, target);
		return;
	}
	seen[target] = n;
	logf("hooked %s variant %d at %p (%s)", name, n, target, addr_str(reinterpret_cast<DWORD64>(target)).c_str());
}
static std::atomic<uint64_t> g_rs_bind_events{0};
// Fallback: ReShade's own bind_descriptor_tables event (layout = ID3D12RootSignature*, table = GPU handle)
static void on_bind_descriptor_tables(command_list *cmd, shader_stage stages, pipeline_layout layout, uint32_t first, uint32_t count, const descriptor_table *tables)
{
	if (!g_recording.load(std::memory_order_relaxed)) return;
	++g_rs_bind_events;
	std::lock_guard lock(g_lbs_mtx);
	auto &s = g_list_binds[cmd->get_native()];
	if ((static_cast<uint32_t>(stages) & static_cast<uint32_t>(shader_stage::compute)) != 0)
	{
		if (s.rootsig != layout.handle) { s.rootsig = layout.handle; memset(s.tables, 0, sizeof(s.tables)); }
		for (uint32_t i = 0; i < count && first + i < 64; ++i) s.tables[first + i] = tables[i].handle;
	}
	else
	{
		if (s.gfx_rootsig != layout.handle) { s.gfx_rootsig = layout.handle; memset(s.gfx_tables, 0, sizeof(s.gfx_tables)); }
		for (uint32_t i = 0; i < count && first + i < 64; ++i) s.gfx_tables[first + i] = tables[i].handle;
	}
}

static void on_bind_pipeline(command_list *cmd, pipeline_stage stages, pipeline pipe)
{
	if ((static_cast<uint32_t>(stages) & static_cast<uint32_t>(pipeline_stage::compute_shader)) != 0)
		gi::on_bind_compute(cmd->get_native(), pipe.handle);
	if (!g_recording.load(std::memory_order_relaxed)) return;
	const uint64_t h = pipe_hash(pipe);
	std::lock_guard lock(g_lbs_mtx);
	auto &s = g_list_binds[cmd->get_native()];
	if ((static_cast<uint32_t>(stages) & static_cast<uint32_t>(pipeline_stage::compute_shader)) != 0) s.compute_pipe = h;
	if ((static_cast<uint32_t>(stages) & static_cast<uint32_t>(pipeline_stage::ray_tracing_shader)) != 0) s.rt_pipe = h;
	if ((static_cast<uint32_t>(stages) & static_cast<uint32_t>(pipeline_stage::pixel_shader)) != 0) s.gfx_pipe = h;
}

static void collect_bindings_native(uint64_t list, std::vector<Binding> &out, std::string &note, bool graphics = false)
{
	ListBindState st;
	{
		std::lock_guard lock(g_lbs_mtx);
		auto it = g_list_binds.find(list);
		if (it == g_list_binds.end()) return;
		st = it->second;
	}
	if (graphics) { st.rootsig = st.gfx_rootsig; memcpy(st.tables, st.gfx_tables, sizeof(st.tables)); }
	std::shared_lock rs_lock(g_rs_mtx);
	auto rit = g_rootsigs.find(st.rootsig);
	if (rit == g_rootsigs.end()) { if (st.rootsig) note = " [rootsig unknown]"; return; }
	const RootSigInfo &rs = rit->second;
	if (rs.flags & 0x400 /* CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED */) note += " [bindless-heap]";

	std::shared_lock heap_lock(g_heap_mtx);
	for (uint32_t p = 0; p < rs.params.size() && p < 64; ++p)
	{
		if (rs.params[p].type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE || !st.tables[p]) continue;
		uint32_t base = 0;
		HeapInfo *h = find_heap(g_heaps_gpu, st.tables[p], &HeapInfo::gpu_start, base);
		if (!h) { note += strf(" [p%u heap?]", p); continue; }
		for (const RootRange &r : rs.params[p].ranges)
		{
			if (r.type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER || r.type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) continue;
			if (r.count == UINT_MAX) { note += strf(" [p%u unbounded %c%u]", p, r.type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV ? 'u' : 't', r.base_reg); continue; }
			const uint32_t n = std::min<uint32_t>(r.count, 32);
			for (uint32_t i = 0; i < n; ++i)
			{
				const uint64_t idx = uint64_t(base) + r.offset + i;
				if (idx >= h->count) break;
				const uint64_t v = h->slots[idx].load(std::memory_order_relaxed);
				if (!v) continue;
				out.push_back({ (uint16_t)p, (uint16_t)(r.base_reg + i), (uint16_t)r.space, r.type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV, res_id(v & ~1ull) });
			}
		}
	}
}

static std::once_flag g_device_hook_once;

static void install_device_hooks(ID3D12Device *dev)
{
	std::call_once(g_device_hook_once, [dev]() {
		void **vt = *reinterpret_cast<void ***>(dev);
		// ID3D12Device vtable: 14 CreateDescriptorHeap, 16 CreateRootSignature, 17 CBV, 18 SRV, 19 UAV, 23 CopyDescriptors, 24 CopyDescriptorsSimple
		bool ok = MH_CreateHook(vt[14], &hk_CreateDescriptorHeap, reinterpret_cast<void **>(&o_CreateDescriptorHeap)) == MH_OK;
		ok &= MH_CreateHook(vt[16], &hk_CreateRootSignature, reinterpret_cast<void **>(&o_CreateRootSignature)) == MH_OK;
		ok &= MH_CreateHook(vt[17], &hk_CreateCBV, reinterpret_cast<void **>(&o_CreateCBV)) == MH_OK;
		ok &= MH_CreateHook(vt[18], &hk_CreateSRV, reinterpret_cast<void **>(&o_CreateSRV)) == MH_OK;
		ok &= MH_CreateHook(vt[19], &hk_CreateUAV, reinterpret_cast<void **>(&o_CreateUAV)) == MH_OK;
		ok &= MH_CreateHook(vt[23], &hk_CopyDescriptors, reinterpret_cast<void **>(&o_CopyDescriptors)) == MH_OK;
		ok &= MH_CreateHook(vt[24], &hk_CopyDescriptorsSimple, reinterpret_cast<void **>(&o_CopyDescriptorsSimple)) == MH_OK;
		if (ok) ok = MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
		logf("native descriptor hooks: %s", ok ? "installed" : "FAILED");
	});
}

static std::map<void *, int> g_seen_scrs, g_seen_scrdt;
static void install_list_bind_hooks(ID3D12GraphicsCommandList *list)
{
	if (!list) return;
	void **vt = *reinterpret_cast<void ***>(list);
	// ID3D12GraphicsCommandList vtable: 29 SetComputeRootSignature, 31 SetComputeRootDescriptorTable
	hook_variant(vt[29], g_scrs_detours, reinterpret_cast<void **>(o_SCRS), g_seen_scrs, "SetComputeRootSignature");
	hook_variant(vt[31], g_scrdt_detours, reinterpret_cast<void **>(o_SCRDT), g_seen_scrdt, "SetComputeRootDescriptorTable");
}
// ---------------------------------------------------------------------------------------------
// Binding resolution (compute / ray tracing root tables) via ReShade trackers (tracking=1 only)
// ---------------------------------------------------------------------------------------------

static void collect_bindings(command_list *cmd, bool ray_tracing, std::vector<Binding> &out)
{
	if (!g_cfg.tracking) return;
	auto *state = cmd->get_private_data<state_tracking>();
	device *dev = cmd->get_device();
	auto *tracker = dev->get_private_data<descriptor_tracking>();
	if (!state || !tracker) return;

	for (const auto &[stages, tables] : state->descriptor_tables)
	{
		const bool is_cs = (static_cast<uint32_t>(stages) & static_cast<uint32_t>(shader_stage::compute)) != 0;
		const bool is_rt = (static_cast<uint32_t>(stages) & static_cast<uint32_t>(shader_stage::all_ray_tracing)) != 0;
		if (!(is_cs || (ray_tracing && is_rt))) continue;

		const pipeline_layout layout = tables.first;
		for (uint32_t p = 0; p < tables.second.size(); ++p)
		{
			const descriptor_table table = tables.second[p];
			if (!table.handle) continue;
			try
			{
				const pipeline_layout_param param = tracker->get_pipeline_layout_param(layout, p);
				uint32_t range_count = 0; const descriptor_range *ranges = nullptr;
				if (param.type == pipeline_layout_param_type::descriptor_table)
				{ range_count = param.descriptor_table.count; ranges = param.descriptor_table.ranges; }
				else if (param.type == pipeline_layout_param_type::descriptor_table_with_static_samplers)
				{ range_count = param.descriptor_table_with_static_samplers.count; ranges = param.descriptor_table_with_static_samplers.ranges; }
				if (!ranges) continue;

				for (uint32_t r = 0; r < range_count; ++r)
				{
					const descriptor_range &range = ranges[r];
					const bool uav = range.type == descriptor_type::texture_unordered_access_view || range.type == descriptor_type::buffer_unordered_access_view;
					const bool srv = range.type == descriptor_type::texture_shader_resource_view || range.type == descriptor_type::buffer_shader_resource_view || range.type == descriptor_type::sampler_with_resource_view;
					if (!uav && !srv) continue;
					const uint32_t n = std::min<uint32_t>(range.count, 32); // unbounded (bindless) arrays are truncated
					for (uint32_t i = 0; i < n; ++i)
					{
						descriptor_heap heap = { 0 }; uint32_t offset = 0;
						dev->get_descriptor_heap_offset(table, range.binding + i, 0, &heap, &offset);
						if (!heap.handle) continue;
						const resource_view view = tracker->get_resource_view(heap, offset);
						if (!view.handle) continue;
						const resource res = dev->get_resource_from_view(view);
						if (!res.handle) continue;
						out.push_back({ (uint16_t)p, (uint16_t)(range.dx_register_index + i), (uint16_t)range.dx_register_space, uav, res_id(res.handle) });
					}
				}
			}
			catch (...) {}
		}
	}
}

static uint64_t bound_pipeline(command_list *cmd, pipeline_stage stage)
{
	if (!g_cfg.tracking)
	{
		std::lock_guard lock(g_lbs_mtx);
		auto it = g_list_binds.find(cmd->get_native());
		if (it == g_list_binds.end()) return 0;
		return stage == pipeline_stage::ray_tracing_shader ? it->second.rt_pipe : it->second.compute_pipe;
	}
	auto *state = cmd->get_private_data<state_tracking>();
	if (!state) return 0;
	for (const auto &[stages, p] : state->pipelines)
		if ((static_cast<uint32_t>(stages) & static_cast<uint32_t>(stage)) != 0)
			return pipe_hash(p);
	return 0;
}

// ---------------------------------------------------------------------------------------------
// ReShade command events (only do work while recording)
// ---------------------------------------------------------------------------------------------

static bool on_dispatch(command_list *cmd, uint32_t x, uint32_t y, uint32_t z)
{
	t_last_list = cmd->get_native();
	if (gi::enabled())
	{
		ID3D12PipelineState *swap = nullptr;
		if (gi::on_dispatch(cmd->get_native(), &swap)) return true; // skipped (GI denoiser bypass)
		if (swap) reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native())->SetPipelineState(swap);
	}
	if (!g_recording.load(std::memory_order_relaxed)) return false;
	Entry e{ EntryKind::dispatch };
	e.pipe_hash = bound_pipeline(cmd, pipeline_stage::compute_shader);
	e.x = x; e.y = y; e.z = z;
	collect_bindings(cmd, false, e.binds);
	if (g_cfg.descriptors) collect_bindings_native(cmd->get_native(), e.binds, e.text);
	push_entry(cmd->get_native(), std::move(e));
	return false;
}

static bool on_dispatch_rays(command_list *cmd, resource, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t, uint32_t w, uint32_t h, uint32_t d)
{
	t_last_list = cmd->get_native();
	if (!g_recording.load(std::memory_order_relaxed)) return false;
	Entry e{ EntryKind::rays };
	e.pipe_hash = bound_pipeline(cmd, pipeline_stage::ray_tracing_shader);
	e.x = w; e.y = h; e.z = d;
	collect_bindings(cmd, true, e.binds);
	if (g_cfg.descriptors) collect_bindings_native(cmd->get_native(), e.binds, e.text);
	push_entry(cmd->get_native(), std::move(e));
	return false;
}

static bool on_indirect(command_list *cmd, indirect_command type, resource, uint64_t, uint32_t draw_count, uint32_t)
{
	t_last_list = cmd->get_native();
	if (!g_recording.load(std::memory_order_relaxed)) return false;
	if (type == indirect_command::dispatch || type == indirect_command::dispatch_rays)
	{
		const bool rays = type == indirect_command::dispatch_rays;
		Entry e{ rays ? EntryKind::rays_indirect : EntryKind::dispatch_indirect };
		e.pipe_hash = bound_pipeline(cmd, rays ? pipeline_stage::ray_tracing_shader : pipeline_stage::compute_shader);
		e.count = draw_count;
		collect_bindings(cmd, rays, e.binds);
		if (g_cfg.descriptors) collect_bindings_native(cmd->get_native(), e.binds, e.text);
		push_entry(cmd->get_native(), std::move(e));
	}
	else
	{
		Entry e{ EntryKind::draws }; e.count = draw_count;
		push_entry(cmd->get_native(), std::move(e));
	}
	return false;
}

static bool on_draw(command_list *cmd, uint32_t, uint32_t, uint32_t, uint32_t)
{
	t_last_list = cmd->get_native();
	if (!g_recording.load(std::memory_order_relaxed)) return false;
	Entry e{ EntryKind::draws }; e.count = 1;
	{
		std::lock_guard lock(g_lbs_mtx);
		auto it = g_list_binds.find(cmd->get_native());
		if (it != g_list_binds.end()) e.pipe_hash = it->second.gfx_pipe;
	}
	bool new_batch = true;
	{
		std::lock_guard lock(g_rec_mtx);
		auto it = g_lists.find(cmd->get_native());
		if (it != g_lists.end() && !it->second.entries.empty() && it->second.entries.back().kind == EntryKind::draws && it->second.entries.back().pipe_hash == e.pipe_hash) new_batch = false;
	}
	if (new_batch && g_cfg.descriptors) collect_bindings_native(cmd->get_native(), e.binds, e.text, true);
	push_entry(cmd->get_native(), std::move(e));
	return false;
}

static bool on_draw_indexed(command_list *cmd, uint32_t, uint32_t, uint32_t, int32_t, uint32_t)
{
	return on_draw(cmd, 0, 0, 0, 0);
}

static std::mutex g_rt0_mtx;
static std::unordered_map<uint64_t, ID3D12Resource *> g_last_rt0; // native list -> last non-null RT0 (debug view only)
static ID3D12Resource *last_rt0(uint64_t list)
{
	std::lock_guard lock(g_rt0_mtx);
	auto it = g_last_rt0.find(list);
	return it != g_last_rt0.end() ? it->second : nullptr;
}

static void on_bind_rts(command_list *cmd, uint32_t count, const resource_view *rtvs, resource_view dsv)
{
	t_last_list = cmd->get_native();
	if (debug_view::mode() != 0 && count >= 1 && rtvs[0].handle)
	{
		auto *r = reinterpret_cast<ID3D12Resource *>(cmd->get_device()->get_resource_from_view(rtvs[0]).handle);
		if (r) { std::lock_guard lock(g_rt0_mtx); g_last_rt0[cmd->get_native()] = r; }
	}
	if (count == 8 && rtvs[3].handle && rtvs[4].handle)
	{
		device *dev = cmd->get_device();
		ID3D12Resource *rts[8] = {};
		for (uint32_t i = 0; i < 8; ++i)
			rts[i] = rtvs[i].handle ? reinterpret_cast<ID3D12Resource *>(dev->get_resource_from_view(rtvs[i]).handle) : nullptr;
		// FH6 G-buffer: RT3 = R32_UINT (normal+smoothness), RT4 = RGBA8 sRGB (albedo)
		if (rts[3] && rts[4] && rts[3]->GetDesc().Format == DXGI_FORMAT_R32_UINT && rts[4]->GetDesc().Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
		{
			debug_view::on_gbuffer(rts);
			rr::on_gbuffer(rts);
		}
	}
	if (!g_recording.load(std::memory_order_relaxed)) return;
	device *dev = cmd->get_device();
	Entry e{ EntryKind::rt_bind };
	for (uint32_t i = 0; i < count; ++i)
		e.rts.push_back(rtvs[i].handle ? res_id(dev->get_resource_from_view(rtvs[i]).handle) : -1);
	e.rts.push_back(dsv.handle ? res_id(dev->get_resource_from_view(dsv).handle) : -1);
	if (count == 0 && !dsv.handle) return;
	push_entry(cmd->get_native(), std::move(e));
}

static void on_reset_command_list(command_list *cmd)
{
	gi::on_reset(cmd->get_native());
	if (!g_recording.load(std::memory_order_relaxed)) return;
	{ std::lock_guard lock(g_lbs_mtx); g_list_binds.erase(cmd->get_native()); }
	std::lock_guard lock(g_rec_mtx);
	g_lists.erase(cmd->get_native());
}

static void on_execute(command_queue *queue, command_list *cmd)
{
	if (!g_recording.load(std::memory_order_relaxed)) return;
	std::lock_guard lock(g_rec_mtx);
	auto it = g_lists.find(cmd->get_native());
	if (g_capture_state.load() == 2)
		g_frame.push_back({ queue->get_native(), cmd->get_native(), it != g_lists.end() ? std::move(it->second) : ListRecord{} });
	if (it != g_lists.end()) g_lists.erase(it);
}

// ---------------------------------------------------------------------------------------------
// PIX marker hooks (ID3D12GraphicsCommandList::SetMarker/BeginEvent/EndEvent)
// ---------------------------------------------------------------------------------------------

using PFN_BeginEvent = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, const void *, UINT);
using PFN_EndEvent = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *);

static std::string decode_marker(UINT metadata, const void *data, UINT size)
{
	if (!data || !size) return {};
	const uint8_t *b = static_cast<const uint8_t *>(data);
	std::string s;
	if (metadata == 0) // UTF-16 string
	{
		const wchar_t *w = static_cast<const wchar_t *>(data);
		for (UINT i = 0; i < size / 2 && w[i]; ++i) s += (w[i] < 128) ? (char)w[i] : '?';
		return s;
	}
	if (metadata == 1) // ANSI string
		return std::string(reinterpret_cast<const char *>(b), strnlen(reinterpret_cast<const char *>(b), size));
	// PIX3 blob: [event info][color][string header][packed chars...][args...]
	if (size > 24)
	{
		const uint8_t *p = b + 24; const UINT n = size - 24;
		bool wide = n >= 4 && p[1] == 0 && p[3] == 0;
		for (UINT i = 0; i < n; i += wide ? 2 : 1)
		{
			const char c = (char)p[i];
			if (c == 0) break;
			s += (c >= 32 && c < 127) ? c : '?';
		}
	}
	return s;
}

static PFN_BeginEvent o_BE[kVariants], o_SM[kVariants];
static PFN_EndEvent o_EE[kVariants];

template <int N>
static void STDMETHODCALLTYPE hk_BE(ID3D12GraphicsCommandList *self, UINT metadata, const void *data, UINT size)
{
	if (debug_view::mode() != 0)
	{
		const std::string name = decode_marker(metadata, data, size);
		if (name.rfind("App::RenderUI", 0) == 0)
			debug_view::draw_ldr(self, last_rt0(reinterpret_cast<uint64_t>(self)));
	}
	if (g_recording.load(std::memory_order_relaxed))
	{
		++g_marker_calls;
		Entry e{ EntryKind::marker_begin }; e.text = decode_marker(metadata, data, size);
		push_entry(reinterpret_cast<uint64_t>(self), std::move(e));
	}
	o_BE[N](self, metadata, data, size);
}
template <int N>
static void STDMETHODCALLTYPE hk_SM(ID3D12GraphicsCommandList *self, UINT metadata, const void *data, UINT size)
{
	if (g_recording.load(std::memory_order_relaxed))
	{
		Entry e{ EntryKind::marker }; e.text = decode_marker(metadata, data, size);
		push_entry(reinterpret_cast<uint64_t>(self), std::move(e));
	}
	o_SM[N](self, metadata, data, size);
}
template <int N>
static void STDMETHODCALLTYPE hk_EE(ID3D12GraphicsCommandList *self)
{
	if (g_recording.load(std::memory_order_relaxed))
		push_entry(reinterpret_cast<uint64_t>(self), Entry{ EntryKind::marker_end });
	o_EE[N](self);
}

static void *const g_be_detours[kVariants] = { (void *)&hk_BE<0>, (void *)&hk_BE<1>, (void *)&hk_BE<2>, (void *)&hk_BE<3>, (void *)&hk_BE<4>, (void *)&hk_BE<5>, (void *)&hk_BE<6>, (void *)&hk_BE<7> };
static void *const g_sm_detours[kVariants] = { (void *)&hk_SM<0>, (void *)&hk_SM<1>, (void *)&hk_SM<2>, (void *)&hk_SM<3>, (void *)&hk_SM<4>, (void *)&hk_SM<5>, (void *)&hk_SM<6>, (void *)&hk_SM<7> };
static void *const g_ee_detours[kVariants] = { (void *)&hk_EE<0>, (void *)&hk_EE<1>, (void *)&hk_EE<2>, (void *)&hk_EE<3>, (void *)&hk_EE<4>, (void *)&hk_EE<5>, (void *)&hk_EE<6>, (void *)&hk_EE<7> };
static std::map<void *, int> g_seen_be, g_seen_sm, g_seen_ee;

static std::mutex g_vt_mtx;
static std::set<uint64_t> g_seen_vtables;
static std::unordered_map<uint64_t, uint64_t> g_cl_to_native; // reshade command_list* -> native list
static std::set<uint64_t> g_native_lists;
static void on_destroy_command_list(command_list *cmd)
{
	std::lock_guard lock(g_vt_mtx);
	auto it = g_cl_to_native.find(reinterpret_cast<uint64_t>(cmd));
	if (it != g_cl_to_native.end()) { g_native_lists.erase(it->second); g_cl_to_native.erase(it); }
}
static void on_init_command_list(command_list *cmd)
{
	auto *native = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native());
	if (!native) return;
	{
		std::lock_guard lock(g_vt_mtx);
		g_cl_to_native[reinterpret_cast<uint64_t>(cmd)] = reinterpret_cast<uint64_t>(native);
		g_native_lists.insert(reinterpret_cast<uint64_t>(native));
	}
	{
		void **vt0 = *reinterpret_cast<void ***>(native);
		std::lock_guard lock(g_vt_mtx);
		if (g_seen_vtables.insert(reinterpret_cast<uint64_t>(vt0)).second && g_seen_vtables.size() <= 16)
			logf("command list class: native=%p vtable=%s type=%d SetComputeRootSignature=%s cmd_list(reshade)=%p", native, addr_str(reinterpret_cast<DWORD64>(vt0)).c_str(), (int)native->GetType(), addr_str(reinterpret_cast<DWORD64>(vt0[29])).c_str(), cmd);
	}
	if (g_cfg.descriptors) install_list_bind_hooks(native);
	if (!g_cfg.markers) return;
	void **vtbl = *reinterpret_cast<void ***>(native);
	// ID3D12GraphicsCommandList vtable: 56 SetMarker, 57 BeginEvent, 58 EndEvent
	hook_variant(vtbl[56], g_sm_detours, reinterpret_cast<void **>(o_SM), g_seen_sm, "SetMarker");
	hook_variant(vtbl[57], g_be_detours, reinterpret_cast<void **>(o_BE), g_seen_be, "BeginEvent");
	hook_variant(vtbl[58], g_ee_detours, reinterpret_cast<void **>(o_EE), g_seen_ee, "EndEvent");
}
// ---------------------------------------------------------------------------------------------
// Streamline hooks
// ---------------------------------------------------------------------------------------------

static PFun_slInit *o_slInit = nullptr;
static PFun_slSetTag *o_slSetTag = nullptr;
static PFun_slSetTagForFrame *o_slSetTagForFrame = nullptr;
static PFun_slEvaluateFeature *o_slEvaluateFeature = nullptr;
static PFun_slSetConstants *o_slSetConstants = nullptr;
static PFun_slGetFeatureFunction *o_slGetFeatureFunction = nullptr;
static PFun_slIsFeatureSupported *o_slIsFeatureSupported = nullptr;
static PFun_slIsFeatureLoaded *o_slIsFeatureLoaded = nullptr;
static PFun_slSetFeatureLoaded *o_slSetFeatureLoaded = nullptr;
static PFun_slDLSSSetOptions *o_slDLSSSetOptions = nullptr;
static PFun_slDLSSGetOptimalSettings *o_slDLSSGetOptimalSettings = nullptr;
static PFun_slDLSSDSetOptions *o_slDLSSDSetOptions = nullptr;

static const char *buffer_type_name(sl::BufferType t)
{
	switch (t)
	{
	case sl::kBufferTypeDepth: return "Depth";
	case sl::kBufferTypeMotionVectors: return "MotionVectors";
	case sl::kBufferTypeHUDLessColor: return "HUDLessColor";
	case sl::kBufferTypeScalingInputColor: return "ScalingInputColor";
	case sl::kBufferTypeScalingOutputColor: return "ScalingOutputColor";
	case sl::kBufferTypeNormals: return "Normals";
	case sl::kBufferTypeRoughness: return "Roughness";
	case sl::kBufferTypeAlbedo: return "Albedo";
	case sl::kBufferTypeSpecularAlbedo: return "SpecularAlbedo";
	case sl::kBufferTypeSpecularMotionVectors: return "SpecularMotionVectors";
	case sl::kBufferTypeExposure: return "Exposure";
	case sl::kBufferTypeNormalRoughness: return "NormalRoughness";
	case sl::kBufferTypeBiasCurrentColorHint: return "BiasCurrentColorHint";
	case sl::kBufferTypeUIColorAndAlpha: return "UIColorAndAlpha";
	case sl::kBufferTypeSpecularHitDistance: return "SpecularHitDistance";
	case sl::kBufferTypeLinearDepth: return "LinearDepth";
	case sl::kBufferTypeBackbuffer: return "Backbuffer";
	case sl::kBufferTypeReactiveMaskHint: return "ReactiveMaskHint";
	case sl::kBufferTypeTransparencyAndCompositionMaskHint: return "TransparencyCompositionMask";
	default: return nullptr;
	}
}

static std::string describe_tag(const sl::ResourceTag &t)
{
	const char *n = buffer_type_name(t.type);
	std::string s = n ? std::string(n) : strf("BufferType%u", t.type);
	if (!t.resource || !t.resource->native)
		return s + " = null";
	auto *r = static_cast<ID3D12Resource *>(t.resource->native);
	s += strf(" = %p [%s] state=0x%x life=%d", r, describe_native(r).c_str(), t.resource->state, (int)t.lifecycle);
	if (t.extent.width) s += strf(" extent=(%u,%u %ux%u)", t.extent.left, t.extent.top, t.extent.width, t.extent.height);
	return s;
}

static std::mutex g_sl_mtx;
static std::map<std::pair<uint32_t, uint32_t>, std::string> g_last_tags; // (viewport, type) -> description
static sl::Constants g_last_constants;
static bool g_have_constants = false;
static std::string g_last_dlss_options, g_last_dlssd_options, g_last_optimal;
static std::map<uint32_t, uint64_t> g_eval_counts;

static void record_sl_note(const std::string &text)
{
	if (g_recording.load(std::memory_order_relaxed) && t_last_list)
	{
		Entry e{ EntryKind::note }; e.text = text;
		push_entry(t_last_list, std::move(e));
	}
}

static void note_tags(uint32_t viewport, const sl::ResourceTag *tags, uint32_t count, const char *fn)
{
	std::lock_guard lock(g_sl_mtx);
	for (uint32_t i = 0; i < count; ++i)
	{
		const std::string d = describe_tag(tags[i]);
		auto &last = g_last_tags[{ viewport, tags[i].type }];
		if (last != d)
		{
			logf("%s vp=%u %s", fn, viewport, d.c_str());
			last = d;
		}
		if (tags[i].resource && tags[i].resource->native && g_recording.load(std::memory_order_relaxed))
			res_id(reinterpret_cast<uint64_t>(tags[i].resource->native));
	}
}

static void install_feature_hooks();

static sl::Result hk_slSetTag(const sl::ViewportHandle &vp, const sl::ResourceTag *tags, uint32_t count, sl::CommandBuffer *cmd)
{
	install_feature_hooks();
	note_tags((uint32_t)vp, tags, count, "slSetTag");
	if (g_recording.load(std::memory_order_relaxed))
	{
		std::string s = "slSetTag:";
		for (uint32_t i = 0; i < count; ++i)
		{
			const char *n = buffer_type_name(tags[i].type);
			s += strf(" %s=R%d", n ? n : "?", tags[i].resource ? res_id((uint64_t)tags[i].resource->native) : -1);
		}
		record_sl_note(s);
	}
	return o_slSetTag(vp, tags, count, cmd);
}

static sl::Result hk_slSetTagForFrame(const sl::FrameToken &frame, const sl::ViewportHandle &vp, const sl::ResourceTag *tags, uint32_t count, sl::CommandBuffer *cmd)
{
	install_feature_hooks();
	note_tags((uint32_t)vp, tags, count, "slSetTagForFrame");
	if (g_recording.load(std::memory_order_relaxed))
	{
		std::string s = "slSetTagForFrame:";
		for (uint32_t i = 0; i < count; ++i)
		{
			const char *n = buffer_type_name(tags[i].type);
			s += strf(" %s=R%d", n ? n : "?", tags[i].resource ? res_id((uint64_t)tags[i].resource->native) : -1);
		}
		record_sl_note(s);
	}
	return o_slSetTagForFrame(frame, vp, tags, count, cmd);
}

static PFun_slGetNativeInterface *s_slGetNativeInterface = nullptr;
// The game passes Streamline its (wrapped) command list. Find the native D3D12 list behind it.
static ID3D12GraphicsCommandList *resolve_native_list(void *cmd, std::string &how)
{
	if (!cmd) { how = "null"; return nullptr; }
	auto lookup = [&](uint64_t p, const char *tag) -> uint64_t {
		std::lock_guard lock(g_vt_mtx);
		if (g_native_lists.count(p)) { how = strf("%s is native", tag); return p; }
		// ReShade proxy: its command_list base sits a few pointers into the COM object
		for (int off = 0; off <= 64; off += 8)
		{
			auto it = g_cl_to_native.find(p + off);
			if (it != g_cl_to_native.end()) { how = strf("%s reshade proxy +%d", tag, off); return it->second; }
		}
		return 0;
	};
	if (uint64_t n = lookup(reinterpret_cast<uint64_t>(cmd), "cmd")) return reinterpret_cast<ID3D12GraphicsCommandList *>(n);
	if (!s_slGetNativeInterface)
		if (HMODULE sl = GetModuleHandleW(L"sl.interposer.dll")) s_slGetNativeInterface = reinterpret_cast<PFun_slGetNativeInterface *>(GetProcAddress(sl, "slGetNativeInterface"));
	if (s_slGetNativeInterface)
	{
		void *base = nullptr;
		if (s_slGetNativeInterface(cmd, &base) == sl::Result::eOk && base)
		{
			if (uint64_t n = lookup(reinterpret_cast<uint64_t>(base), "sl-base")) return reinterpret_cast<ID3D12GraphicsCommandList *>(n);
			how = strf("sl-base %p not matched", base);
			return nullptr;
		}
	}
	how = "no match";
	return nullptr;
}

static sl::Result hk_slEvaluateFeature(sl::Feature feature, const sl::FrameToken &frame, const sl::BaseStructure **inputs, uint32_t num, sl::CommandBuffer *cmd)
{
	install_feature_hooks();
	bool first = false;
	{
		std::lock_guard lock(g_sl_mtx);
		first = g_eval_counts[feature]++ == 0;
	}
	std::string s = strf("slEvaluateFeature feature=%u inputs=%u cmd=%p", feature, num, cmd);
	for (uint32_t i = 0; i < num; ++i)
	{
		for (const sl::BaseStructure *b = inputs[i]; b; b = b->next)
		{
			if (b->structType == sl::ViewportHandle::s_structType)
				s += strf(" [viewport %u]", (uint32_t)*static_cast<const sl::ViewportHandle *>(b));
			else if (b->structType == sl::ResourceTag::s_structType)
			{
				const auto *t = static_cast<const sl::ResourceTag *>(b);
				s += " [tag " + describe_tag(*t) + "]";
				note_tags(0xFFFFFFFF, t, 1, "evaluate-local-tag");
			}
			else
			{
				const auto &g = b->structType;
				s += strf(" [struct %08x-%04x v%zu]", g.data1, g.data2, b->structVersion);
			}
		}
	}
	if (first) logf("FIRST %s", s.c_str());
	record_sl_note(s);
	if (feature == sl::kFeatureDLSS && rr::enabled())
	{
		std::string how;
		ID3D12GraphicsCommandList *native = resolve_native_list(cmd, how);
		if (rr::evaluate(frame, inputs, num, cmd, native))
			return sl::Result::eOk; // RR replaced DLSS-SR this frame
	}
	return o_slEvaluateFeature(feature, frame, inputs, num, cmd);
}

static sl::Result hk_slSetConstants(const sl::Constants &c, const sl::FrameToken &frame, const sl::ViewportHandle &vp)
{
	{
		std::lock_guard lock(g_sl_mtx);
		const bool first = !g_have_constants;
		memcpy(&g_last_constants, &c, std::min<size_t>(sizeof(sl::Constants), c.structVersion >= 2 ? sizeof(sl::Constants) : offsetof(sl::Constants, motionVectorsJittered) + sizeof(sl::Boolean)));
		g_have_constants = true;
		if (c.structVersion >= 2) rr::on_constants(c);
		if (first)
			logf("first slSetConstants vp=%u version=%zu", (uint32_t)vp, c.structVersion);
	}
	return o_slSetConstants(c, frame, vp);
}

static std::string describe_dlss_options(const sl::DLSSOptions &o)
{
	static const char *modes[] = { "Off", "MaxPerformance", "Balanced", "MaxQuality", "UltraPerformance", "UltraQuality", "DLAA" };
	const uint32_t m = (uint32_t)o.mode;
	return strf("mode=%s out=%ux%u preExposure=%.3f exposureScale=%.3f HDR=%d autoExposure=%d presets(dlaa=%u q=%u b=%u p=%u up=%u) v%zu",
		m < 7 ? modes[m] : "?", o.outputWidth, o.outputHeight, o.preExposure, o.exposureScale, (int)o.colorBuffersHDR,
		o.structVersion >= 3 ? (int)o.useAutoExposure : -1,
		(uint32_t)o.dlaaPreset, (uint32_t)o.qualityPreset, (uint32_t)o.balancedPreset, (uint32_t)o.performancePreset, (uint32_t)o.ultraPerformancePreset, o.structVersion);
}

static sl::Result hk_slDLSSSetOptions(const sl::ViewportHandle &vp, const sl::DLSSOptions &o)
{
	rr::on_dlss_options(o);
	const std::string s = strf("vp=%u ", (uint32_t)vp) + describe_dlss_options(o);
	{
		std::lock_guard lock(g_sl_mtx);
		if (s != g_last_dlss_options) { logf("slDLSSSetOptions %s", s.c_str()); g_last_dlss_options = s; }
	}
	return o_slDLSSSetOptions(vp, o);
}

static sl::Result hk_slDLSSGetOptimalSettings(const sl::DLSSOptions &o, sl::DLSSOptimalSettings &out)
{
	const sl::Result r = o_slDLSSGetOptimalSettings(o, out);
	const std::string s = describe_dlss_options(o) + strf(" -> render %ux%u (min %ux%u max %ux%u) result=%d",
		out.optimalRenderWidth, out.optimalRenderHeight, out.renderWidthMin, out.renderHeightMin, out.renderWidthMax, out.renderHeightMax, (int)r);
	std::lock_guard lock(g_sl_mtx);
	if (s != g_last_optimal) { logf("slDLSSGetOptimalSettings %s", s.c_str()); g_last_optimal = s; }
	return r;
}

static sl::Result hk_slDLSSDSetOptions(const sl::ViewportHandle &vp, const sl::DLSSDOptions &o)
{
	const std::string s = strf("vp=%u mode=%u out=%ux%u v%zu", (uint32_t)vp, (uint32_t)o.mode, o.outputWidth, o.outputHeight, o.structVersion);
	{
		std::lock_guard lock(g_sl_mtx);
		if (s != g_last_dlssd_options) { logf("slDLSSDSetOptions (game uses RR!) %s", s.c_str()); g_last_dlssd_options = s; }
	}
	return o_slDLSSDSetOptions(vp, o);
}

// Plugin functions are wrapped by handing the game our wrapper when it asks for them.
// (Never detour the returned pointers directly: several of them share code, and patching one
//  redirected slDLSSGetState into the wrong hook, which corrupted the game's DLSSState and crashed.)
// Adds DLSS Ray Reconstruction (dlss_d) to the plugins the game asks Streamline to load.
static sl::Result hk_slInit(const sl::Preferences &pref, uint64_t sdk_version)
{
	std::string list;
	for (uint32_t i = 0; i < pref.numFeaturesToLoad && pref.featuresToLoad; ++i) list += strf(" %u", pref.featuresToLoad[i]);
	logf("slInit: sdk=0x%llx flags=0x%llx features=[%s ] engine=%d", (unsigned long long)sdk_version, (unsigned long long)pref.flags, list.c_str(), (int)pref.engine);
	if (!pref.featuresToLoad || !pref.numFeaturesToLoad)
		return o_slInit(pref, sdk_version); // null list = load everything
	static std::vector<sl::Feature> features;
	features.assign(pref.featuresToLoad, pref.featuresToLoad + pref.numFeaturesToLoad);
	if (std::find(features.begin(), features.end(), sl::kFeatureDLSS_RR) == features.end())
		features.push_back(sl::kFeatureDLSS_RR);
	sl::Preferences p = pref;
	p.featuresToLoad = features.data();
	p.numFeaturesToLoad = (uint32_t)features.size();
	const sl::Result r = o_slInit(p, sdk_version);
	logf("slInit with DLSS_RR added -> %d", (int)r);
	return r;
}

static sl::Result hk_slGetFeatureFunction(sl::Feature feature, const char *name, void *&fn)
{
	const sl::Result r = o_slGetFeatureFunction(feature, name, fn);
	logf("slGetFeatureFunction feature=%u name=%s result=%d fn=%p", feature, name ? name : "(null)", (int)r, fn);
	if (r != sl::Result::eOk || !fn || !name)
		return r;
	if (feature == sl::kFeatureDLSS && strcmp(name, "slDLSSSetOptions") == 0)
	{
		o_slDLSSSetOptions = reinterpret_cast<PFun_slDLSSSetOptions *>(fn);
		fn = reinterpret_cast<void *>(&hk_slDLSSSetOptions);
	}
	else if (feature == sl::kFeatureDLSS && strcmp(name, "slDLSSGetOptimalSettings") == 0)
	{
		o_slDLSSGetOptimalSettings = reinterpret_cast<PFun_slDLSSGetOptimalSettings *>(fn);
		fn = reinterpret_cast<void *>(&hk_slDLSSGetOptimalSettings);
	}
	else if (feature == sl::kFeatureDLSS_RR && strcmp(name, "slDLSSDSetOptions") == 0)
	{
		o_slDLSSDSetOptions = reinterpret_cast<PFun_slDLSSDSetOptions *>(fn);
		fn = reinterpret_cast<void *>(&hk_slDLSSDSetOptions);
	}
	return r;
}

static sl::Result hk_slIsFeatureSupported(sl::Feature feature, const sl::AdapterInfo &info)
{
	const sl::Result r = o_slIsFeatureSupported(feature, info);
	logf("slIsFeatureSupported feature=%u -> %d", feature, (int)r);
	return r;
}

static sl::Result hk_slIsFeatureLoaded(sl::Feature feature, bool &loaded)
{
	const sl::Result r = o_slIsFeatureLoaded(feature, loaded);
	static std::set<uint32_t> seen;
	std::lock_guard lock(g_sl_mtx);
	if (seen.insert(feature).second) logf("slIsFeatureLoaded feature=%u -> %d loaded=%d", feature, (int)r, (int)loaded);
	return r;
}

static sl::Result hk_slSetFeatureLoaded(sl::Feature feature, bool loaded)
{
	logf("slSetFeatureLoaded feature=%u loaded=%d", feature, (int)loaded);
	return o_slSetFeatureLoaded(feature, loaded);
}

template <typename T>
static bool hook_export(HMODULE mod, const char *name, T *detour, T **original)
{
	void *target = reinterpret_cast<void *>(GetProcAddress(mod, name));
	if (!target) { logf("  export %s not found", name); return false; }
	if (MH_CreateHook(target, reinterpret_cast<void *>(detour), reinterpret_cast<void **>(original)) != MH_OK) { logf("  hook %s FAILED", name); return false; }
	MH_EnableHook(target);
	return true;
}

static std::atomic<bool> g_sl_hooked{false};
static std::atomic<bool> g_feature_hooked{false};
static std::mutex g_hook_mtx;

static void try_hook_streamline()
{
	if (g_sl_hooked.load() || !g_cfg.streamline) return;
	std::lock_guard lock(g_hook_mtx);
	if (g_sl_hooked.load()) return;
	HMODULE sl = GetModuleHandleW(L"sl.interposer.dll");
	if (!sl) return;

	wchar_t path[MAX_PATH] = {}; GetModuleFileNameW(sl, path, MAX_PATH);
	char pathA[MAX_PATH] = {}; WideCharToMultiByte(CP_UTF8, 0, path, -1, pathA, MAX_PATH, nullptr, nullptr);
	logf("Hooking Streamline: %s", pathA);
	hook_export(sl, "slInit", &hk_slInit, &o_slInit);
	hook_export(sl, "slSetTag", &hk_slSetTag, &o_slSetTag);
	hook_export(sl, "slSetTagForFrame", &hk_slSetTagForFrame, &o_slSetTagForFrame);
	hook_export(sl, "slEvaluateFeature", &hk_slEvaluateFeature, &o_slEvaluateFeature);
	hook_export(sl, "slSetConstants", &hk_slSetConstants, &o_slSetConstants);
	hook_export(sl, "slGetFeatureFunction", &hk_slGetFeatureFunction, &o_slGetFeatureFunction);
	hook_export(sl, "slIsFeatureSupported", &hk_slIsFeatureSupported, &o_slIsFeatureSupported);
	hook_export(sl, "slIsFeatureLoaded", &hk_slIsFeatureLoaded, &o_slIsFeatureLoaded);
	hook_export(sl, "slSetFeatureLoaded", &hk_slSetFeatureLoaded, &o_slSetFeatureLoaded);
	g_sl_hooked = true;
	rr::SlFunctions fns;
	fns.set_tag_for_frame = o_slSetTagForFrame;
	fns.evaluate = o_slEvaluateFeature;
	fns.get_feature_function = o_slGetFeatureFunction;
	fns.is_feature_loaded = o_slIsFeatureLoaded;
	rr::set_sl(fns);
}

// Read-only status queries, done once from the first SL call (after slInit ran).
static void install_feature_hooks()
{
	if (g_feature_hooked.load(std::memory_order_relaxed) || !o_slGetFeatureFunction) return;
	std::lock_guard lock(g_hook_mtx);
	if (g_feature_hooked.load()) return;
	g_feature_hooked = true;

	logf("DLSS SetOptions wrapped: %s", o_slDLSSSetOptions ? "yes" : "no (game fetched it before the addon loaded)");

	bool loaded = false;
	if (o_slIsFeatureLoaded)
	{
		const sl::Result r = o_slIsFeatureLoaded(sl::kFeatureDLSS_RR, loaded);
		logf("DLSS-RR (dlss_d) plugin loaded by game: result=%d loaded=%d", (int)r, (int)loaded);
		const sl::Result r2 = o_slIsFeatureLoaded(sl::kFeatureDLSS, loaded);
		logf("DLSS-SR plugin loaded by game: result=%d loaded=%d", (int)r2, (int)loaded);
	}
}

// LoadLibrary hook so we catch sl.interposer even if it loads after us
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
// Capture dump
// ---------------------------------------------------------------------------------------------

static const char *mat_row(const sl::float4x4 &m, int r, char *buf)
{
	const sl::float4 &v = m[r];
	snprintf(buf, 128, "% .4f % .4f % .4f % .4f", v.x, v.y, v.z, v.w);
	return buf;
}

static void write_capture()
{
	std::vector<Executed> frame;
	{
		std::lock_guard lock(g_rec_mtx);
		frame.swap(g_frame);
		g_lists.clear();
	}

	const int index = g_capture_index++;
	const auto file_path = g_dir / ("fh6rr_capture_" + std::to_string(index) + ".txt");
	FILE *f = _wfopen(file_path.c_str(), L"w");
	if (!f) { logf("capture: could not open output file"); return; }

	const auto shader_dir = g_dir / "fh6rr_shaders";
	std::error_code ec; std::filesystem::create_directories(shader_dir, ec);

	fprintf(f, "FH6 RR recon capture %d (frame %llu)\n", index, (unsigned long long)g_frame_counter);
	fprintf(f, "descriptor tracking: %llu heaps, %llu views, %llu copies, %zu root signatures\nduring capture: %llu SetComputeRootSignature, %llu SetComputeRootDescriptorTable, %llu BeginEvent, %llu reshade bind_descriptor_tables; variants hooked: rootsig=%zu table=%zu marker=%zu\n\n", (unsigned long long)g_heap_count.load(), (unsigned long long)g_view_count.load(), (unsigned long long)g_copy_count.load(), g_rootsigs.size(), (unsigned long long)g_scrs_calls.load(), (unsigned long long)g_scrdt_calls.load(), (unsigned long long)g_marker_calls.load(), (unsigned long long)g_rs_bind_events.load(), g_seen_scrs.size(), g_seen_scrdt.size(), g_seen_be.size());

	{
		std::lock_guard lock(g_sl_mtx);
		fprintf(f, "== Streamline state ==\n");
		fprintf(f, "DLSS options: %s\n", g_last_dlss_options.c_str());
		fprintf(f, "DLSS optimal: %s\n", g_last_optimal.c_str());
		fprintf(f, "DLSS-RR options: %s\n", g_last_dlssd_options.empty() ? "(never set by game)" : g_last_dlssd_options.c_str());
		for (const auto &[feat, n] : g_eval_counts) fprintf(f, "evaluate feature %u: %llu calls so far\n", feat, (unsigned long long)n);
		fprintf(f, "Latest tags:\n");
		for (const auto &[key, d] : g_last_tags) fprintf(f, "  vp=%u %s\n", key.first, d.c_str());
		if (g_have_constants)
		{
			const sl::Constants &c = g_last_constants;
			char b[128];
			fprintf(f, "Constants: jitter=(%.4f, %.4f) mvecScale=(%.6f, %.6f) near=%.4f far=%.1f fov=%.4f aspect=%.4f\n",
				c.jitterOffset.x, c.jitterOffset.y, c.mvecScale.x, c.mvecScale.y, c.cameraNear, c.cameraFar, c.cameraFOV, c.cameraAspectRatio);
			fprintf(f, "  depthInverted=%d cameraMotionIncluded=%d mv3D=%d reset=%d mvDilated=%d mvJittered=%d\n",
				(int)c.depthInverted, (int)c.cameraMotionIncluded, (int)c.motionVectors3D, (int)c.reset, (int)c.motionVectorsDilated, (int)c.motionVectorsJittered);
			fprintf(f, "  cameraPos=(%.2f %.2f %.2f) fwd=(%.3f %.3f %.3f) up=(%.3f %.3f %.3f)\n",
				c.cameraPos.x, c.cameraPos.y, c.cameraPos.z, c.cameraFwd.x, c.cameraFwd.y, c.cameraFwd.z, c.cameraUp.x, c.cameraUp.y, c.cameraUp.z);
			fprintf(f, "  cameraViewToClip:\n"); for (int r = 0; r < 4; ++r) fprintf(f, "    %s\n", mat_row(c.cameraViewToClip, r, b));
			fprintf(f, "  clipToPrevClip:\n"); for (int r = 0; r < 4; ++r) fprintf(f, "    %s\n", mat_row(c.clipToPrevClip, r, b));
		}
		fprintf(f, "\n");
	}

	std::set<uint64_t> used_shaders;
	size_t dispatches = 0, rays = 0, draws = 0;

	fprintf(f, "== Frame (submission order) ==\n");
	for (const Executed &ex : frame)
	{
		if (ex.rec.entries.empty()) continue;
		fprintf(f, "\n-- ExecuteCommandLists queue=%llx list=%llx (%zu entries)\n", (unsigned long long)ex.queue, (unsigned long long)ex.list, ex.rec.entries.size());
		int depth = 0;
		for (const Entry &e : ex.rec.entries)
		{
			if (e.kind == EntryKind::marker_end) { depth = std::max(0, depth - 1); continue; }
			fprintf(f, "%*s", 2 + depth * 2, "");
			switch (e.kind)
			{
			case EntryKind::marker_begin: fprintf(f, "> %s\n", e.text.c_str()); ++depth; break;
			case EntryKind::marker: fprintf(f, "* %s\n", e.text.c_str()); break;
			case EntryKind::note: fprintf(f, "[SL] %s\n", e.text.c_str()); break;
			case EntryKind::draws:
			{
				fprintf(f, "draws x%u ps=%016llx", e.count, (unsigned long long)e.pipe_hash); draws += e.count;
				if (e.pipe_hash) used_shaders.insert(e.pipe_hash);
				std::string srv;
				for (const Binding &b : e.binds) srv += strf(" %c%u%s=R%d", b.uav ? 'u' : 't', b.reg, b.space ? strf(",s%u", b.space).c_str() : "", b.res);
				if (!srv.empty()) fprintf(f, "  BIND:%s", srv.c_str());
				fprintf(f, "\n");
				break;
			}
			case EntryKind::rt_bind:
			{
				fprintf(f, "RT:");
				for (size_t i = 0; i + 1 < e.rts.size(); ++i) fprintf(f, " R%d", e.rts[i]);
				fprintf(f, " DS: R%d\n", e.rts.back());
				break;
			}
			default:
			{
				const char *kind = e.kind == EntryKind::dispatch ? "CS" : e.kind == EntryKind::dispatch_indirect ? "CS-indirect" : e.kind == EntryKind::rays ? "RAYS" : "RAYS-indirect";
				if (e.kind == EntryKind::rays || e.kind == EntryKind::rays_indirect) ++rays; else ++dispatches;
				if (e.kind == EntryKind::dispatch || e.kind == EntryKind::rays)
					fprintf(f, "%s %016llx [%u,%u,%u]", kind, (unsigned long long)e.pipe_hash, e.x, e.y, e.z);
				else
					fprintf(f, "%s %016llx (count %u)", kind, (unsigned long long)e.pipe_hash, e.count);
				if (e.pipe_hash) used_shaders.insert(e.pipe_hash);
				std::string srv, uav;
				for (const Binding &b : e.binds)
					(b.uav ? uav : srv) += strf(" %c%u%s=R%d", b.uav ? 'u' : 't', b.reg, b.space ? strf(",s%u", b.space).c_str() : "", b.res);
				if (!srv.empty()) fprintf(f, "  SRV:%s", srv.c_str());
				if (!uav.empty()) fprintf(f, "  UAV:%s", uav.c_str());
				if (!e.text.empty()) fprintf(f, " %s", e.text.c_str());
				fprintf(f, "\n");
				break;
			}
			}
		}
	}

	fprintf(f, "\n== Resources ==\n");
	{
		std::lock_guard lock(g_res_mtx);
		for (size_t i = 0; i < g_res_list.size(); ++i)
			fprintf(f, "R%zu %p %s\n", i, (void *)g_res_list[i].first, g_res_list[i].second.c_str());
		// Registry is per-capture
		g_res_ids.clear();
		g_res_list.clear();
	}

	size_t written = 0;
	{
		std::lock_guard lock(g_code_mtx);
		for (const uint64_t h : used_shaders)
		{
			auto it = g_shader_code.find(h);
			if (it == g_shader_code.end() || it->second.second.empty()) continue;
			const auto p = shader_dir / strf("%016llx_%c.bin", (unsigned long long)h, it->second.first);
			if (std::filesystem::exists(p, ec)) continue;
			if (FILE *sf = _wfopen(p.c_str(), L"wb")) { fwrite(it->second.second.data(), 1, it->second.second.size(), sf); fclose(sf); ++written; }
		}
	}

	fprintf(f, "\nSummary: %zu compute dispatches, %zu ray dispatches, %zu draws, %zu command lists, %zu new shader files\n", dispatches, rays, draws, frame.size(), written);
	fclose(f);
	logf("Capture %d written: %zu dispatches, %zu ray dispatches, %zu draws, %zu shaders", index, dispatches, rays, draws, written);
}

// ---------------------------------------------------------------------------------------------
// Frame boundary
// ---------------------------------------------------------------------------------------------

static bool g_f9_down = false, g_f8_down = false, g_f7_down = false, g_f6_down = false, g_f5_down = false;

static void on_present(command_queue *, swapchain *, const rect *, const rect *, uint32_t, const rect *)
{
	++g_frame_counter;
	try_hook_streamline();
	{
		const bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
		if (f8 && !g_f8_down) debug_view::next_mode();
		debug_view::new_frame();
		const bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
		if (f7 && !g_f7_down) rr::toggle();
		g_f7_down = f7;
		const bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
		if (f6 && !g_f6_down) gi::toggle();
		g_f6_down = f6;
		const bool f5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
		if (f5 && !g_f5_down) gi::next_scale();
		g_f5_down = f5;
		g_f8_down = f8;
	}

	switch (g_capture_state.load())
	{
	case 0:
	{
		const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
		if (down && !g_f9_down)
		{
			logf("F9: capture armed");
			g_scrs_calls = 0; g_scrdt_calls = 0; g_marker_calls = 0; g_rs_bind_events = 0;
			{ std::lock_guard lock(g_rec_mtx); g_lists.clear(); g_frame.clear(); }
			g_recording = true;
			g_capture_state = 1;
		}
		g_f9_down = down;
		break;
	}
	case 1: // warm-up frame finished, record the next one completely
	{
		std::lock_guard lock(g_rec_mtx);
		g_frame.clear();
		g_capture_state = 2;
		break;
	}
	case 2:
		g_recording = false;
		g_capture_state = 0;
		write_capture();
		break;
	}
}

static void on_init_device(device *dev)
{
	logf("init_device api=0x%x", (unsigned)dev->get_api());
	if (g_cfg.descriptors) install_device_hooks(reinterpret_cast<ID3D12Device *>(dev->get_native()));
	debug_view::on_device(reinterpret_cast<ID3D12Device *>(dev->get_native()));
	rr::on_device(reinterpret_cast<ID3D12Device *>(dev->get_native()));
	try_hook_streamline();
}

// ---------------------------------------------------------------------------------------------
// Crash logger: records fatal-looking first-chance exceptions with module+offset and a stack
// ---------------------------------------------------------------------------------------------

static std::string addr_str(DWORD64 addr)
{
	HMODULE mod = nullptr;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(addr), &mod) && mod)
	{
		wchar_t path[MAX_PATH] = {}; GetModuleFileNameW(mod, path, MAX_PATH);
		const wchar_t *base = wcsrchr(path, L'\\'); base = base ? base + 1 : path;
		char name[MAX_PATH] = {}; WideCharToMultiByte(CP_UTF8, 0, base, -1, name, MAX_PATH, nullptr, nullptr);
		return strf("%s+0x%llx", name, (unsigned long long)(addr - reinterpret_cast<DWORD64>(mod)));
	}
	return strf("0x%llx", (unsigned long long)addr);
}

static std::atomic<int> g_crash_logs{0};

static LONG CALLBACK crash_handler(EXCEPTION_POINTERS *ep)
{
	const DWORD code = ep->ExceptionRecord->ExceptionCode;
	if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
		code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != 0xC0000409 /* fail fast */ && code != 0xC0000374 /* heap corruption */)
		return EXCEPTION_CONTINUE_SEARCH;
	if (g_crash_logs.fetch_add(1) >= 30)
		return EXCEPTION_CONTINUE_SEARCH;

	std::string s = strf("EXCEPTION 0x%08lx at %s thread=%lu", code, addr_str(reinterpret_cast<DWORD64>(ep->ExceptionRecord->ExceptionAddress)).c_str(), GetCurrentThreadId());
	if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
		s += strf(" (%s 0x%llx)", ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "read" : ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" : "execute", (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
	logf("%s", s.c_str());

	// Walk the stack from the faulting context
	CONTEXT ctx = *ep->ContextRecord;
	for (int i = 0; i < 32 && ctx.Rip; ++i)
	{
		logf("    #%02d %s", i, addr_str(ctx.Rip).c_str());
		DWORD64 image_base = 0;
		PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
		if (!fn)
		{
			// Leaf function: return address is at RSP
			if (!ctx.Rsp || IsBadReadPtr(reinterpret_cast<void *>(ctx.Rsp), 8)) break;
			ctx.Rip = *reinterpret_cast<DWORD64 *>(ctx.Rsp);
			ctx.Rsp += 8;
			continue;
		}
		void *handler_data = nullptr; DWORD64 establisher = 0;
		RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &establisher, nullptr);
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------------------------
// Config (fh6rr.ini next to the addon) so parts can be switched off to isolate problems
// ---------------------------------------------------------------------------------------------



static void load_config()
{
	const auto ini = g_dir / "fh6rr.ini";
	if (!std::filesystem::exists(ini))
	{
		if (FILE *f = _wfopen(ini.c_str(), L"w"))
		{
			fputs("; FH6 RR recon switches (1 = on, 0 = off). Restart the game after editing.\n[recon]\nstreamline=1\nmarkers=1\ntracking=0\npipelines=1\nloadlibrary=1\ncommands=1\ndescriptors=1\n", f);
			fclose(f);
		}
	}
	auto get = [&](const wchar_t *key, bool def) { return GetPrivateProfileIntW(L"recon", key, def ? 1 : 0, ini.c_str()) != 0; };
	g_cfg.streamline = get(L"streamline", true);
	g_cfg.markers = get(L"markers", true);
	g_cfg.tracking = get(L"tracking", false);
	g_cfg.pipelines = get(L"pipelines", true);
	g_cfg.loadlibrary = get(L"loadlibrary", true);
	g_cfg.commands = get(L"commands", true);
	g_cfg.descriptors = get(L"descriptors", true);
	logf("config: streamline=%d markers=%d tracking=%d pipelines=%d loadlibrary=%d commands=%d descriptors=%d",
		g_cfg.streamline, g_cfg.markers, g_cfg.tracking, g_cfg.pipelines, g_cfg.loadlibrary, g_cfg.commands, g_cfg.descriptors);
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
		g_module = module;
		if (!reshade::register_addon(module))
			return FALSE;

		wchar_t path[MAX_PATH] = {};
		GetModuleFileNameW(module, path, MAX_PATH);
		g_dir = std::filesystem::path(path).parent_path();
		g_log = _wfopen((g_dir / "fh6rr_recon.log").c_str(), L"w");
		logf("FH6 RR recon loaded. Press F9 in-game to capture a frame.");
		// NOTE: no vectored exception handler. FH6 relies on guard-page exceptions (0x80000001) and
		// crashes at startup when a foreign VEH is registered.
		(void)&crash_handler;
		load_config();

		MH_Initialize();
		HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
		if (kb && g_cfg.loadlibrary)
		{
			void *ll = reinterpret_cast<void *>(GetProcAddress(kb, "LoadLibraryExW"));
			if (ll && MH_CreateHook(ll, &hk_LoadLibraryExW, reinterpret_cast<void **>(&o_LoadLibraryExW)) == MH_OK)
				MH_EnableHook(ll);
		}

		if (g_cfg.tracking)
		{
			descriptor_tracking::register_events();
			state_tracking::register_events();
		}
		reshade::register_event<reshade::addon_event::init_device>(on_init_device);
		if (g_cfg.pipelines)
		{
			reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
			reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
		}
		reshade::register_event<reshade::addon_event::init_command_list>(on_init_command_list);
		reshade::register_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
		if (g_cfg.pipelines && !g_cfg.tracking)
			reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
		if (g_cfg.descriptors && !g_cfg.tracking)
			reshade::register_event<reshade::addon_event::bind_descriptor_tables>(on_bind_descriptor_tables);
		if (g_cfg.commands)
		{
			reshade::register_event<reshade::addon_event::reset_command_list>(on_reset_command_list);
			reshade::register_event<reshade::addon_event::execute_command_list>(on_execute);
			reshade::register_event<reshade::addon_event::dispatch>(on_dispatch);
			reshade::register_event<reshade::addon_event::dispatch_rays>(on_dispatch_rays);
			reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_indirect);
			reshade::register_event<reshade::addon_event::draw>(on_draw);
			reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
			reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_rts);
		}
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
