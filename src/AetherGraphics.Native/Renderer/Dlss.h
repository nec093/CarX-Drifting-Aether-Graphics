// NVIDIA DLSS (NGX, SDK 310.9.16) for the scene's temporal resolve: DLAA (the render size is the output size) and
// Super Resolution (the scene is drawn smaller; DLSS writes the output size). SceneRenderer decides the render size
// with RenderSize, draws up to the resolve at that size, and Evaluate records DLSS in the TemporalAA pass's place.
//
// NGX is initialised once per device (NVSDK_NGX_D3D12_Init_with_ProjectID, a custom engine); nvngx_dlss.dll is
// looked for in the mod's native folder (beside the shaders folder) and the runtime's own folder. When anything is
// missing (another GPU vendor, an old driver, the DLL), Available() stays false, it is logged once, and the renderer
// keeps its own TAA. The feature is made again when the mode or a size changes; the old one is released once the GPU
// is past the frame that used it.
// Ray Reconstruction (EvaluateRr) is the path tracer's denoiser: the same NGX instance, a feature of its own
// (NVSDK_NGX_Feature_RayReconstruction from nvngx_dlssd.dll), checked at EnsureInit like super sampling.
// Built only with the SDK (AETHER_HAS_DLSS, build\deps\DLSS-310.9.16). Render thread only, except Stats / Available.
#pragma once
#include "../Core/Common.h"
#include "../Core/Json.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace aether {

class Dlss {
public:
	enum Mode { kOff = -1, kDlaa = 0, kQuality, kBalanced, kPerformance, kUltraPerformance, kModeCount };
	static Dlss& Get();
	static bool Built();                             // built with the SDK
	static Mode ModeFromSetting(const std::string& upscaler); // taa (or anything else): kOff
	static const char* ModeName(Mode mode);

	// NGX on this device (once; later calls return the first answer). False: DLSS is not usable (logged once).
	bool EnsureInit(ID3D12Device* device);
	bool Available() const { return available_.load(); }    // any thread
	bool InitTried() const { return initTried_.load(); }   // any thread: EnsureInit has run on a device

	// The size to draw the scene at for `mode` and this output size (DLAA: the output size). False: not available.
	bool RenderSize(Mode mode, uint32_t outWidth, uint32_t outHeight, uint32_t& width, uint32_t& height);

	struct Inputs {
		ID3D12Resource* color = nullptr;   // the scene, linear HDR before exposure and tone mapping (render size)
		ID3D12Resource* depth = nullptr;   // reversed Z (render size)
		ID3D12Resource* motion = nullptr;  // RG: where the pixel was minus where it is, in uv units, without the jitter (render size)
		ID3D12Resource* output = nullptr;  // UAV, output size
		uint32_t renderWidth = 0, renderHeight = 0, outWidth = 0, outHeight = 0;
		float jitterX = 0, jitterY = 0;    // the projection's sub pixel offset in render pixels (x right, y down)
		bool reset = false;                // the history does not apply (another camera, a cut)
		float frameMs = 16.7f;
	};
	// Records the feature's creation (when the mode or a size changed) and its evaluation. The inputs are in
	// NON_PIXEL_SHADER_RESOURCE, the output in UNORDERED_ACCESS. NGX binds its own heaps and root signature: every
	// later pass binds its own again. False: nothing was written (`note` says why).
	bool Evaluate(ID3D12GraphicsCommandList* list, Mode mode, const Inputs& in, UINT64 retireFence, std::string& note);
	// ---- DLSS Ray Reconstruction (DLSS-D, nvngx_dlssd.dll): the path tracer's denoiser. Its render size is the output
	// size (DLAA-like: no upscaling). Available once EnsureInit has run and NGX reports SuperSamplingDenoising.
	bool RrAvailable() const { return rrAvailable_.load(); } // any thread
	std::string RrReason();                                  // any thread: why it is not available ("" when it is)
	struct RrInputs {
		ID3D12Resource* color = nullptr;          // noisy linear HDR (before exposure), one fresh sample per pixel
		ID3D12Resource* diffuseAlbedo = nullptr;  // RGB [0, 1]
		ID3D12Resource* specularAlbedo = nullptr; // RGB [0, 1]: the preintegrated specular reflectance (F0 * A + B)
		ID3D12Resource* normalRoughness = nullptr; // xyz world space normal, w linear (perceptual) roughness (packed)
		ID3D12Resource* specularHitDistance = nullptr; // R: world space distance to the first specular bounce's hit (optional)
		ID3D12Resource* depth = nullptr;          // reversed Z hardware depth
		ID3D12Resource* motion = nullptr;         // as Inputs::motion
		ID3D12Resource* output = nullptr;         // UAV, the same size
		uint32_t width = 0, height = 0;
		float jitterX = 0, jitterY = 0;           // render pixels, as Inputs
		const float* worldToView = nullptr;       // 16 floats, row-vector convention (v * M), row major
		const float* viewToClip = nullptr;        // 16 floats, the same convention, without the jitter
		bool reset = false;
		float frameMs = 16.7f;
	};
	// As Evaluate, for Ray Reconstruction (its own feature, made again when the size changes).
	bool EvaluateRr(ID3D12GraphicsCommandList* list, const RrInputs& in, UINT64 retireFence, std::string& note);

	// Every frame: releases the features the GPU is done with.
	void Collect(UINT64 completedFence);
	// Device release: every feature and NGX itself (leak: the GPU may still use them; nothing is called into NGX).
	void Release(bool leak);
	json Stats();

private:
	void NoteCall(const char* call, uint32_t result); // the last NGX call and its result, for Stats
	struct Impl;
	Impl* impl_ = nullptr;
	std::atomic<bool> available_{false}, initTried_{false}, rrAvailable_{false};
	std::mutex statsMutex_;
	json stats_ = json::object();
	json rrStats_ = json::object(); // Ray Reconstruction's part of Stats ("rr")
};

} // namespace aether
