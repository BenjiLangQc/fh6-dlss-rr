#pragma once
#include <d3d12.h>

// Phase 3 step 1: on-screen view of decoded G-buffer guides.
// Drawn into the final LDR image (after tonemapping) right when the game starts drawing its HUD
// ("App::RenderUI" PIX marker), on the native command list. Pure native D3D12.

namespace debug_view
{
	void on_device(ID3D12Device *device);
	// Called whenever the game binds its 8-target G-buffer MRT (native resources, any may be null).
	void on_gbuffer(ID3D12Resource *const rts[8]);
	// Records the debug dispatch into `target` (state RENDER_TARGET) on a native command list.
	// Draws at most once per frame. Returns true if it drew.
	bool draw_ldr(ID3D12GraphicsCommandList *cmd, ID3D12Resource *target);
	void new_frame();
	void next_mode();
	int mode();
	const char *mode_name(int mode);
}
