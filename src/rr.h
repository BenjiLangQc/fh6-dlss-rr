#pragma once
#include <d3d12.h>
#include <sl.h>
#include <sl_dlss.h>
#include <sl_dlss_d.h>

// Phase 3 step 2: DLSS Ray Reconstruction through Streamline's own dlss_d plugin.
// The game does not request the plugin; recon.cpp adds kFeatureDLSS_RR to slInit's feature list.
// When enabled (F7), the game's DLSS-SR evaluate is replaced by a guide pass + DLSS-RR evaluate.

namespace rr
{
	struct SlFunctions
	{
		PFun_slSetTagForFrame *set_tag_for_frame = nullptr;
		PFun_slEvaluateFeature *evaluate = nullptr;
		PFun_slGetFeatureFunction *get_feature_function = nullptr;
		PFun_slIsFeatureLoaded *is_feature_loaded = nullptr;
	};

	void set_sl(const SlFunctions &fns);
	void on_device(ID3D12Device *device);
	// `depth`: the depth buffer bound with the G-buffer MRT (diagnostics: which frame the set belongs to).
	void on_gbuffer(ID3D12Resource *const rts[8], ID3D12Resource *depth = nullptr);
	// Depth buffer the game tags for DLSS (from slSetTag / slSetTagForFrame / evaluate inputs).
	void on_dlss_depth(ID3D12Resource *depth);
	void on_constants(const sl::Constants &constants);
	void on_dlss_options(const sl::DLSSOptions &options);

	bool enabled();
	void set_enabled(bool on);
	void toggle();
	// RR model requested through Streamline: 0 = game/driver default (NVIDIA app override applies), 4 = preset D
	// (default transformer), 5 = preset E (latest transformer). Streamline 2.11 maps presets F..O back to default.
	int preset();
	void set_preset(int preset);

	// Guide debug options (bitmask, live): test variants of the G-buffer decode for the RR guides.
	// Bits 0-1: F0 mode. 0 = 0.04 everywhere (default: no seams after RR), 1 = the game's rule (bit5 -> RT4 alpha,
	// else bit0 clear -> RT6, else 0.04), 2 = the old decode (RT6 wherever bit0 is clear, ignoring bit5).
	constexpr uint32_t k_guide_f0_mask       = 3;
	constexpr uint32_t k_guide_rt5_occlusion = 4; // diffuse albedo *= RT5 alpha (the game's texture occlusion)
	constexpr uint32_t k_guide_full_diffuse  = 8; // diffuse albedo = RT4 as is (no (1 - F0) reduction)
	uint32_t guide_options();
	void set_guide_options(uint32_t options);
	// Render and output size of the last successful RR evaluate; false before the first one.
	bool last_sizes(uint32_t &render_w, uint32_t &render_h, uint32_t &output_w, uint32_t &output_h);

	// Called from the slEvaluateFeature(kFeatureDLSS) hook. Returns true when RR was evaluated instead
	// (the caller must then NOT run DLSS-SR). `native` is the unwrapped command list.
	bool evaluate(const sl::FrameToken &frame, const sl::BaseStructure **inputs, uint32_t num_inputs,
		sl::CommandBuffer *cmd, ID3D12GraphicsCommandList *native);
}
