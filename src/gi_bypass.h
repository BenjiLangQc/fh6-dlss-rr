#pragma once
#include <d3d12.h>
#include <cstdint>

// Phase 3 step 3: replaces FH6's RT GI denoiser with the raw 1-ray signal so DLSS-RR does the denoising.
//   temporal pass ea12dd2b624b6962      -> skipped
//   spatial pass  6e9ffce615b836da #1   -> skipped
//   spatial pass  6e9ffce615b836da #2   -> runs with OUR pipeline state (game root signature + tables)
//   resolve pass  30f774e7ce6a460c      -> untouched, now resolves the raw signal
// No D3D12 state other than the pipeline state object is touched, so the game's state stays valid.

namespace gi
{
	// From init_pipeline: native device, native root signature, shader hash, pipeline handle.
	void on_pipeline(ID3D12Device *device, ID3D12RootSignature *root_signature, uint64_t hash, uint64_t pipeline);
	void on_destroy_pipeline(uint64_t pipeline);
	// From bind_pipeline (compute stage only).
	void on_bind_compute(uint64_t list, uint64_t pipeline);
	// From dispatch. Returns true when the dispatch must be skipped.
	bool on_dispatch(uint64_t list, ID3D12GraphicsCommandList *native);
	void on_reset(uint64_t list);

	bool enabled();
	void set_enabled(bool on);
	void toggle();
	void next_scale();
	void set_scale(int index);
	void set_ao(int index);   // 0 = off, 1 = soft 0.25 m, 2 = soft 1 m, 3 = hard 1.5 m
	void next_ao();
	// The bypass only acts while this is true (the mod sets it when DLSS-RR succeeded on recent frames),
	// so raw noisy GI never reaches the screen without RR denoising it.
	void set_allowed(bool allowed);
}
