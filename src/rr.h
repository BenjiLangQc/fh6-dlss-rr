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
	void on_gbuffer(ID3D12Resource *const rts[8]);
	void on_constants(const sl::Constants &constants);
	void on_dlss_options(const sl::DLSSOptions &options);

	bool enabled();
	void set_enabled(bool on);
	void toggle();

	// Called from the slEvaluateFeature(kFeatureDLSS) hook. Returns true when RR was evaluated instead
	// (the caller must then NOT run DLSS-SR). `native` is the unwrapped command list.
	bool evaluate(const sl::FrameToken &frame, const sl::BaseStructure **inputs, uint32_t num_inputs,
		sl::CommandBuffer *cmd, ID3D12GraphicsCommandList *native);
}
