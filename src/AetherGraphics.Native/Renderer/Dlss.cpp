#include "Dlss.h"

#include "../Core/Log.h"
#include "../Diagnostics.h"
#include "../Runtime.h"

#include <algorithm>
#include <cmath>

#if AETHER_HAS_DLSS
// (not nvsdk_ngx_helpers.h: it pulls in the CUDA helpers, which are not fetched; its one function used here is below)
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_d3d.h>
// Ray Reconstruction's helpers (NGX_D3D12_CREATE_DLSSD_EXT / NGX_D3D12_EVALUATE_DLSSD_EXT) include nvsdk_ngx_helpers.h
// for its D3D part only: its guard is set so that the CUDA helpers it would pull in are left out
#ifndef NVSDK_NGX_HELPERS_H
#define NVSDK_NGX_HELPERS_H
#endif
#include <nvsdk_ngx_helpers_dlssd_d3d.h>
#endif

namespace aether {

Dlss& Dlss::Get() {
	static Dlss* d = new Dlss();
	return *d;
}

Dlss::Mode Dlss::ModeFromSetting(const std::string& upscaler) {
	if (upscaler == "dlaa") return kDlaa;
	if (upscaler == "dlss_quality") return kQuality;
	if (upscaler == "dlss_balanced") return kBalanced;
	if (upscaler == "dlss_performance") return kPerformance;
	if (upscaler == "dlss_ultra_performance") return kUltraPerformance;
	return kOff;
}

const char* Dlss::ModeName(Mode mode) {
	switch (mode) {
	case kDlaa: return "dlaa";
	case kQuality: return "dlss_quality";
	case kBalanced: return "dlss_balanced";
	case kPerformance: return "dlss_performance";
	case kUltraPerformance: return "dlss_ultra_performance";
	default: return "off";
	}
}

#if !AETHER_HAS_DLSS

bool Dlss::Built() { return false; }
bool Dlss::EnsureInit(ID3D12Device*) {
	initTried_.store(true);
	return false;
}
bool Dlss::RenderSize(Mode, uint32_t, uint32_t, uint32_t&, uint32_t&) { return false; }
bool Dlss::Evaluate(ID3D12GraphicsCommandList*, Mode, const Inputs&, UINT64, std::string& note) {
	note = "built without DLSS";
	return false;
}
bool Dlss::EvaluateRr(ID3D12GraphicsCommandList*, const RrInputs&, UINT64, std::string& note) {
	note = "built without DLSS";
	return false;
}
std::string Dlss::RrReason() { return "built without the DLSS SDK"; }
void Dlss::Collect(UINT64) {}
void Dlss::Release(bool) {}
json Dlss::Stats() {
	return {{"built", false}, {"available", false}, {"reason", "built without the DLSS SDK (scripts/fetch-deps.ps1: build\\deps\\DLSS-310.9.16)"}, {"rr", {{"available", false}, {"created", false}}}};
}

#else

namespace {

// (NGX wants a GUID-like id for an engine of its own: Aether Graphics' own, fixed)
const char* const kProjectId = "6a0e3f52-9c1b-4d7e-8f25-3b4a1c9d7e60";

std::string ResultText(NVSDK_NGX_Result r) {
	const wchar_t* text = GetNGXResultAsString(r);
	return Format("0x%08X", unsigned(r)) + (text ? " " + WideToUtf8(text) : std::string());
}

NVSDK_NGX_PerfQuality_Value PerfQuality(Dlss::Mode mode) {
	switch (mode) {
	case Dlss::kQuality: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
	case Dlss::kBalanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
	case Dlss::kPerformance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
	case Dlss::kUltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
	default: return NVSDK_NGX_PerfQuality_Value_DLAA;
	}
}

// The render size DLSS wants for an output size and mode (what the SDK's NGX_DLSS_GET_OPTIMAL_SETTINGS does).
NVSDK_NGX_Result OptimalRenderSize(NVSDK_NGX_Parameter* params, uint32_t outWidth, uint32_t outHeight, NVSDK_NGX_PerfQuality_Value quality, unsigned int& width, unsigned int& height) {
	void* callback = nullptr;
	NVSDK_NGX_Parameter_GetVoidPointer(params, NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &callback);
	if (!callback) {
		return NVSDK_NGX_Result_FAIL_OutOfDate; // (the DLSS in use is too old for the call)
	}
	NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_Width, outWidth);
	NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_Height, outHeight);
	NVSDK_NGX_Parameter_SetI(params, NVSDK_NGX_Parameter_PerfQualityValue, quality);
	NVSDK_NGX_Parameter_SetI(params, NVSDK_NGX_Parameter_RTXValue, 0);
	const NVSDK_NGX_Result r = reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback>(callback)(params);
	if (NVSDK_NGX_FAILED(r)) {
		return r;
	}
	NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_OutWidth, &width);
	NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_OutHeight, &height);
	return r;
}

std::wstring ModuleDirectory() {
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&ModuleDirectory), &self);
	wchar_t buffer[MAX_PATH] = {};
	GetModuleFileNameW(self, buffer, MAX_PATH);
	std::wstring path = buffer;
	const size_t slash = path.find_last_of(L"\\/");
	return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

} // namespace

struct Dlss::Impl {
	ID3D12Device* device = nullptr;
	bool initialised = false;     // NGX is up on `device`
	NVSDK_NGX_Parameter* params = nullptr;
	NVSDK_NGX_Handle* feature = nullptr;
	Mode featureMode = kOff;
	uint32_t featureRw = 0, featureRh = 0, featureOw = 0, featureOh = 0;
	bool featureFailed = false;   // (a creation that failed is not tried again every frame for the same sizes)
	struct Retired { NVSDK_NGX_Handle* handle; UINT64 fence; };
	std::vector<Retired> retired;
	// RenderSize's last answer
	Mode sizeMode = kOff;
	uint32_t sizeOw = 0, sizeOh = 0, sizeRw = 0, sizeRh = 0;
	std::vector<std::wstring> paths;
	std::vector<const wchar_t*> pathPointers;
	int needsDriver = 0, minDriverMajor = 0, minDriverMinor = 0;
	std::string dllFolder;
	// Ray Reconstruction
	NVSDK_NGX_Handle* rrFeature = nullptr;
	uint32_t rrW = 0, rrH = 0;
	bool rrFailed = false;
};

bool Dlss::Built() { return true; }

bool Dlss::EnsureInit(ID3D12Device* device) {
	if (!impl_) {
		impl_ = new Impl();
	}
	Impl& d = *impl_;
	if (initTried_.load() && d.device == device) {
		return available_.load();
	}
	initTried_.store(true);
	d.device = device;
	available_.store(false);
	auto fail = [&](const std::string& why, NVSDK_NGX_Result r) {
		(void)r;
		AE_LOG_CODE(Warning, "Dlss", "W_DLSS_UNAVAILABLE", "DLSS is not available (%s): the scene keeps Aether's TAA", why.c_str());
		std::lock_guard lock(statsMutex_);
		stats_["reason"] = why;
		return false;
	};
	if (!device) {
		return fail("no device", NVSDK_NGX_Result_FAIL_InvalidParameter);
	}
	// where nvngx_dlss.dll is: the mod's native folder (the runtime itself runs from the game's plugin folder), then the
	// runtime's own folder (the test host, the build output)
	d.paths.clear();
	const std::wstring data = Runtime::Get().DataDir();
	if (!data.empty()) {
		d.paths.push_back(data + L"\\native");
	}
	d.paths.push_back(ModuleDirectory());
	d.pathPointers.clear();
	for (const std::wstring& p : d.paths) {
		d.pathPointers.push_back(p.c_str());
		if (d.dllFolder.empty() && GetFileAttributesW((p + L"\\nvngx_dlss.dll").c_str()) != INVALID_FILE_ATTRIBUTES) {
			d.dllFolder = WideToUtf8(p);
		}
	}
	if (d.dllFolder.empty()) {
		return fail("nvngx_dlss.dll is not next to the native runtime", NVSDK_NGX_Result_FAIL_FeatureNotFound);
	}
	NVSDK_NGX_FeatureCommonInfo info = {};
	info.PathListInfo.Path = d.pathPointers.data();
	info.PathListInfo.Length = uint32_t(d.pathPointers.size());
	const std::wstring writable = data.empty() ? ModuleDirectory() : data;
	NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kAetherVersion, writable.c_str(), device, &info, NVSDK_NGX_Version_API);
	NoteCall("NVSDK_NGX_D3D12_Init_with_ProjectID", unsigned(r));
	if (NVSDK_NGX_FAILED(r)) {
		return fail("NGX did not start: " + ResultText(r) + " (not an NVIDIA RTX GPU, or the driver is too old)", r);
	}
	d.initialised = true;
	r = NVSDK_NGX_D3D12_GetCapabilityParameters(&d.params);
	NoteCall("NVSDK_NGX_D3D12_GetCapabilityParameters", unsigned(r));
	if (NVSDK_NGX_FAILED(r) || !d.params) {
		fail("no NGX capability parameters: " + ResultText(r), r);
		Release(false);
		initTried_.store(true);
		d.device = device; // (the answer stands for this device)
		return false;
	}
	NVSDK_NGX_Parameter_GetI(d.params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &d.needsDriver);
	unsigned int major = 0, minor = 0;
	NVSDK_NGX_Parameter_GetUI(d.params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
	NVSDK_NGX_Parameter_GetUI(d.params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
	d.minDriverMajor = int(major);
	d.minDriverMinor = int(minor);
	int available = 0;
	r = NVSDK_NGX_Parameter_GetI(d.params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
	if (NVSDK_NGX_FAILED(r) || !available) {
		int initResult = 0;
		NVSDK_NGX_Parameter_GetI(d.params, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
		std::string why = d.needsDriver ? Format("the driver is too old (DLSS needs %d.%d or newer)", d.minDriverMajor, d.minDriverMinor)
			: "DLSS super sampling is not available on this GPU: " + ResultText(NVSDK_NGX_Result(initResult));
		fail(why, NVSDK_NGX_Result(initResult));
		Release(false);
		initTried_.store(true);
		d.device = device; // (the answer stands for this device)
		return false;
	}
	available_.store(true);
	AE_LOG(Info, "Dlss", "DLSS is available (NGX, SDK 310.9.16; nvngx_dlss.dll from %s)", d.dllFolder.c_str());
	// Ray Reconstruction: its own DLL (nvngx_dlssd.dll, beside nvngx_dlss.dll) and its own capability
	std::string rrWhy;
	bool rrDll = false;
	for (const std::wstring& p : d.paths) {
		rrDll = rrDll || GetFileAttributesW((p + L"\\nvngx_dlssd.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
	}
	if (!rrDll) {
		rrWhy = "nvngx_dlssd.dll is not next to the native runtime";
	} else {
		int rrAvailable = 0, rrNeedsDriver = 0;
		unsigned int rrMajor = 0, rrMinor = 0;
		NVSDK_NGX_Parameter_GetI(d.params, NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, &rrNeedsDriver);
		NVSDK_NGX_Parameter_GetUI(d.params, NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMajor, &rrMajor);
		NVSDK_NGX_Parameter_GetUI(d.params, NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMinor, &rrMinor);
		r = NVSDK_NGX_Parameter_GetI(d.params, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &rrAvailable);
		if (NVSDK_NGX_FAILED(r) || !rrAvailable) {
			int initResult = 0;
			NVSDK_NGX_Parameter_GetI(d.params, NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult, &initResult);
			rrWhy = rrNeedsDriver ? Format("the driver is too old (Ray Reconstruction needs %u.%u or newer)", rrMajor, rrMinor)
				: "Ray Reconstruction is not available on this GPU: " + ResultText(NVSDK_NGX_Result(initResult));
		}
	}
	rrAvailable_.store(rrWhy.empty());
	if (rrWhy.empty()) {
		AE_LOG(Info, "Dlss", "DLSS Ray Reconstruction is available (nvngx_dlssd.dll): the path tracer's denoiser");
	} else {
		AE_LOG_CODE(Warning, "Dlss", "W_DLSS_RR_UNAVAILABLE", "DLSS Ray Reconstruction is not available (%s): path tracing averages its samples", rrWhy.c_str());
	}
	std::lock_guard lock(statsMutex_);
	stats_.erase("reason");
	stats_["dllFolder"] = d.dllFolder;
	if (rrWhy.empty()) {
		rrStats_.erase("reason");
	} else {
		rrStats_["reason"] = rrWhy;
	}
	return true;
}

bool Dlss::RenderSize(Mode mode, uint32_t outWidth, uint32_t outHeight, uint32_t& width, uint32_t& height) {
	if (!impl_ || !available_.load() || mode == kOff || outWidth == 0 || outHeight == 0) {
		return false;
	}
	Impl& d = *impl_;
	if (mode == kDlaa) {
		width = outWidth;
		height = outHeight;
		return true;
	}
	if (d.sizeMode == mode && d.sizeOw == outWidth && d.sizeOh == outHeight) {
		if (!d.sizeRw) {
			return false; // (asked before: no answer for this size)
		}
		width = d.sizeRw;
		height = d.sizeRh;
		return true;
	}
	unsigned int rw = 0, rh = 0;
	const NVSDK_NGX_Result r = OptimalRenderSize(d.params, outWidth, outHeight, PerfQuality(mode), rw, rh);
	NoteCall("DLSS optimal settings", unsigned(r));
	if (NVSDK_NGX_FAILED(r) || rw == 0 || rh == 0 || rw > outWidth || rh > outHeight) {
		AE_LOG_CODE(Warning, "Dlss", "W_DLSS_SIZE", "DLSS gave no render size for %s at %ux%u (%s)", ModeName(mode), outWidth, outHeight, ResultText(r).c_str());
		d.sizeMode = mode;
		d.sizeOw = outWidth;
		d.sizeOh = outHeight;
		d.sizeRw = d.sizeRh = 0;
		return false;
	}
	d.sizeMode = mode;
	d.sizeOw = outWidth;
	d.sizeOh = outHeight;
	d.sizeRw = rw;
	d.sizeRh = rh;
	width = rw;
	height = rh;
	return true;
}

bool Dlss::Evaluate(ID3D12GraphicsCommandList* list, Mode mode, const Inputs& in, UINT64 retireFence, std::string& note) {
	if (!impl_ || !available_.load() || mode == kOff || !list || !in.color || !in.depth || !in.motion || !in.output) {
		note = "not available";
		return false;
	}
	Impl& d = *impl_;
	bool created = false;
	if (!d.feature || d.featureMode != mode || d.featureRw != in.renderWidth || d.featureRh != in.renderHeight || d.featureOw != in.outWidth || d.featureOh != in.outHeight) {
		const bool sameAsFailed = d.featureFailed && d.featureMode == mode && d.featureRw == in.renderWidth && d.featureRh == in.renderHeight && d.featureOw == in.outWidth && d.featureOh == in.outHeight;
		if (sameAsFailed) {
			note = "the DLSS feature could not be made for these sizes";
			return false;
		}
		if (d.feature) {
			d.retired.push_back({d.feature, retireFence});
			d.feature = nullptr;
		}
		d.featureMode = mode;
		d.featureRw = in.renderWidth;
		d.featureRh = in.renderHeight;
		d.featureOw = in.outWidth;
		d.featureOh = in.outHeight;
		// the transformer presets the 310 SDK names as the defaults: K for DLAA, Quality and Balanced, M for Performance,
		// L for Ultra Performance (set explicitly: "default" may change with the driver's updates)
		NVSDK_NGX_Parameter_SetUI(d.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_DLSS_Hint_Render_Preset_K);
		NVSDK_NGX_Parameter_SetUI(d.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, NVSDK_NGX_DLSS_Hint_Render_Preset_K);
		NVSDK_NGX_Parameter_SetUI(d.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_DLSS_Hint_Render_Preset_K);
		NVSDK_NGX_Parameter_SetUI(d.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, NVSDK_NGX_DLSS_Hint_Render_Preset_M);
		NVSDK_NGX_Parameter_SetUI(d.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_DLSS_Hint_Render_Preset_L);
		NVSDK_NGX_DLSS_Create_Params create = {};
		create.Feature.InWidth = in.renderWidth;
		create.Feature.InHeight = in.renderHeight;
		create.Feature.InTargetWidth = in.outWidth;
		create.Feature.InTargetHeight = in.outHeight;
		create.Feature.InPerfQualityValue = PerfQuality(mode);
		// the input is the scene's linear HDR picture before exposure; motion at the render size, without the jitter;
		// reversed Z; DLSS meters the exposure itself
		create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
			NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
		create.InEnableOutputSubrects = false;
		const NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(list, 1, 1, &d.feature, d.params, &create);
			NoteCall("NGX_D3D12_CREATE_DLSS_EXT", unsigned(r));
		if (NVSDK_NGX_FAILED(r) || !d.feature) {
			d.feature = nullptr;
			d.featureFailed = true;
			AE_LOG_CODE(Warning, "Dlss", "W_DLSS_CREATE", "The DLSS feature could not be made (%s, %ux%u -> %ux%u): %s", ModeName(mode), in.renderWidth, in.renderHeight, in.outWidth, in.outHeight,
				ResultText(r).c_str());
			note = "feature creation failed: " + ResultText(r);
			return false;
		}
		d.featureFailed = false;
		created = true;
		{
			std::lock_guard lock(statsMutex_);
			stats_["featuresCreated"] = stats_.value("featuresCreated", 0) + 1;
		}
		AE_LOG(Info, "Dlss", "DLSS feature made: %s, %ux%u -> %ux%u", ModeName(mode), in.renderWidth, in.renderHeight, in.outWidth, in.outHeight);
	}
	NVSDK_NGX_D3D12_DLSS_Eval_Params e = {};
	e.Feature.pInColor = in.color;
	e.Feature.pInOutput = in.output;
	e.Feature.InSharpness = 0.0f; // (deprecated in 310: not used)
	e.pInDepth = in.depth;
	e.pInMotionVectors = in.motion;
	e.InJitterOffsetX = in.jitterX;
	e.InJitterOffsetY = in.jitterY;
	e.InRenderSubrectDimensions = {in.renderWidth, in.renderHeight};
	e.InReset = in.reset || created ? 1 : 0;
	// the motion texture holds (where it was - where it is) in uv units: NGX wants render pixels
	e.InMVScaleX = float(in.renderWidth);
	e.InMVScaleY = float(in.renderHeight);
	e.InFrameTimeDeltaInMsec = in.frameMs;
	e.InPreExposure = 1.0f;
	e.InExposureScale = 1.0f;
	const NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(list, d.feature, d.params, &e);
	NoteCall("NGX_D3D12_EVALUATE_DLSS_EXT", unsigned(r));
	if (NVSDK_NGX_FAILED(r)) {
		AE_LOG_CODE(Warning, "Dlss", "W_DLSS_EVALUATE", "DLSS evaluation failed (%s): %s", ModeName(mode), ResultText(r).c_str());
		note = "evaluation failed: " + ResultText(r);
		return false;
	}
	note = Format("%s, %ux%u -> %ux%u, jitter %+.3f %+.3f px%s", ModeName(mode), in.renderWidth, in.renderHeight, in.outWidth, in.outHeight, in.jitterX, in.jitterY, e.InReset ? ", reset" : "");
	std::lock_guard lock(statsMutex_);
	stats_["mode"] = ModeName(mode);
	stats_["renderSize"] = {in.renderWidth, in.renderHeight};
	stats_["outputSize"] = {in.outWidth, in.outHeight};
	stats_["lastReset"] = e.InReset != 0;
	return true;
}

std::string Dlss::RrReason() {
	if (rrAvailable_.load()) {
		return {};
	}
	std::lock_guard lock(statsMutex_);
	if (rrStats_.contains("reason")) {
		return rrStats_["reason"].get<std::string>();
	}
	if (stats_.contains("reason")) {
		return "DLSS is not available: " + stats_["reason"].get<std::string>();
	}
	return initTried_.load() ? "not available" : "not asked yet";
}

bool Dlss::EvaluateRr(ID3D12GraphicsCommandList* list, const RrInputs& in, UINT64 retireFence, std::string& note) {
	if (!impl_ || !available_.load() || !rrAvailable_.load() || !list || !in.color || !in.diffuseAlbedo || !in.specularAlbedo || !in.normalRoughness || !in.depth || !in.motion || !in.output ||
		!in.worldToView || !in.viewToClip || in.width == 0 || in.height == 0) {
		note = "not available";
		return false;
	}
	Impl& d = *impl_;
	bool created = false;
	if (!d.rrFeature || d.rrW != in.width || d.rrH != in.height) {
		if (d.rrFailed && d.rrW == in.width && d.rrH == in.height) {
			note = "the Ray Reconstruction feature could not be made for this size";
			return false;
		}
		if (d.rrFeature) {
			d.retired.push_back({d.rrFeature, retireFence});
			d.rrFeature = nullptr;
		}
		d.rrW = in.width;
		d.rrH = in.height;
		// (the preset: the driver's default for the mode. The 310 SDK names D and E the transformer models and marks the
		// later letters "do not use")
		NVSDK_NGX_Parameter_SetUI(d.params, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA, NVSDK_NGX_RayReconstruction_Hint_Render_Preset_Default);
		NVSDK_NGX_DLSSD_Create_Params create = {};
		create.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
		create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed; // (the roughness in the normals' w)
		create.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
		create.InWidth = in.width;
		create.InHeight = in.height;
		create.InTargetWidth = in.width;
		create.InTargetHeight = in.height;
		create.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
		// as super sampling: linear HDR before exposure, motion at the render size without the jitter, reversed Z,
		// exposure metered by DLSS itself
		create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
			NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
		create.InEnableOutputSubrects = false;
		const NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSSD_EXT(list, 1, 1, &d.rrFeature, d.params, &create);
		NoteCall("NGX_D3D12_CREATE_DLSSD_EXT", unsigned(r));
		if (NVSDK_NGX_FAILED(r) || !d.rrFeature) {
			d.rrFeature = nullptr;
			d.rrFailed = true;
			AE_LOG_CODE(Warning, "Dlss", "W_DLSS_RR_CREATE", "The DLSS Ray Reconstruction feature could not be made (%ux%u): %s", in.width, in.height, ResultText(r).c_str());
			note = "Ray Reconstruction feature creation failed: " + ResultText(r);
			std::lock_guard lock(statsMutex_);
			rrStats_["created"] = false;
			rrStats_["lastResult"] = ResultText(r);
			rrStats_["lastResultOk"] = false;
			return false;
		}
		d.rrFailed = false;
		created = true;
		AE_LOG(Info, "Dlss", "DLSS Ray Reconstruction feature made: %ux%u", in.width, in.height);
		std::lock_guard lock(statsMutex_);
		rrStats_["created"] = true;
		rrStats_["featuresCreated"] = rrStats_.value("featuresCreated", 0) + 1;
	}
	NVSDK_NGX_D3D12_DLSSD_Eval_Params e = {};
	e.pInColor = in.color;
	e.pInOutput = in.output;
	e.pInDepth = in.depth;
	e.pInMotionVectors = in.motion;
	e.pInDiffuseAlbedo = in.diffuseAlbedo;
	e.pInSpecularAlbedo = in.specularAlbedo;
	e.pInNormals = in.normalRoughness;
	e.pInRoughness = nullptr; // (packed into the normals' w)
	e.pInSpecularHitDistance = in.specularHitDistance;
	e.InJitterOffsetX = in.jitterX;
	e.InJitterOffsetY = in.jitterY;
	e.InRenderSubrectDimensions = {in.width, in.height};
	e.InReset = in.reset || created ? 1 : 0;
	// (the motion texture holds where it was minus where it is in uv units: NGX wants render pixels)
	e.InMVScaleX = float(in.width);
	e.InMVScaleY = float(in.height);
	e.InFrameTimeDeltaInMsec = in.frameMs;
	e.InPreExposure = 1.0f;
	e.InExposureScale = 1.0f;
	e.pInWorldToViewMatrix = const_cast<float*>(in.worldToView);
	e.pInViewToClipMatrix = const_cast<float*>(in.viewToClip);
	const NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSSD_EXT(list, d.rrFeature, d.params, &e);
	NoteCall("NGX_D3D12_EVALUATE_DLSSD_EXT", unsigned(r));
	std::lock_guard lock(statsMutex_);
	rrStats_["lastResult"] = ResultText(r);
	rrStats_["lastResultOk"] = !NVSDK_NGX_FAILED(r);
	if (NVSDK_NGX_FAILED(r)) {
		AE_LOG_CODE(Warning, "Dlss", "W_DLSS_RR_EVALUATE", "DLSS Ray Reconstruction evaluation failed: %s", ResultText(r).c_str());
		note = "Ray Reconstruction evaluation failed: " + ResultText(r);
		return false;
	}
	rrStats_["renderSize"] = {in.width, in.height};
	rrStats_["lastReset"] = e.InReset != 0;
	rrStats_["evaluations"] = rrStats_.value("evaluations", uint64_t(0)) + 1;
	note = Format("DLSS Ray Reconstruction, %ux%u, jitter %+.3f %+.3f px%s%s", in.width, in.height, in.jitterX, in.jitterY, in.specularHitDistance ? ", specular hit distance" : "",
		e.InReset ? ", reset" : "");
	return true;
}

void Dlss::Collect(UINT64 completedFence) {
	if (!impl_) {
		return;
	}
	Impl& d = *impl_;
	for (size_t i = 0; i < d.retired.size();) {
		if (d.retired[i].fence <= completedFence) {
			NVSDK_NGX_D3D12_ReleaseFeature(d.retired[i].handle);
			d.retired.erase(d.retired.begin() + ptrdiff_t(i));
		} else {
			++i;
		}
	}
}

void Dlss::Release(bool leak) {
	if (!impl_) {
		return;
	}
	Impl& d = *impl_;
	if (!leak && d.initialised) {
		for (const Impl::Retired& r : d.retired) {
			NVSDK_NGX_D3D12_ReleaseFeature(r.handle);
		}
		if (d.feature) {
			NVSDK_NGX_D3D12_ReleaseFeature(d.feature);
		}
		if (d.rrFeature) {
			NVSDK_NGX_D3D12_ReleaseFeature(d.rrFeature);
		}
		if (d.params) {
			NVSDK_NGX_D3D12_DestroyParameters(d.params);
		}
		if (d.device) {
			NVSDK_NGX_D3D12_Shutdown1(d.device);
		}
	}
	d.retired.clear();
	d.feature = nullptr;
	d.rrFeature = nullptr;
	d.rrW = d.rrH = 0;
	d.rrFailed = false;
	d.params = nullptr;
	d.initialised = false;
	d.device = nullptr;
	d.featureMode = kOff;
	d.featureRw = d.featureRh = d.featureOw = d.featureOh = 0;
	d.featureFailed = false;
	d.dllFolder.clear();
	d.sizeMode = kOff;
	d.sizeRw = d.sizeRh = 0;
	available_.store(false);
	rrAvailable_.store(false);
	initTried_.store(false); // (a new device asks again)
	std::lock_guard lock(statsMutex_);
	rrStats_ = json::object();
	stats_.erase("mode");
	stats_.erase("renderSize");
	stats_.erase("outputSize");
}

void Dlss::NoteCall(const char* call, uint32_t result) {
	std::lock_guard lock(statsMutex_);
	stats_["lastCall"] = call;
	stats_["lastResult"] = ResultText(NVSDK_NGX_Result(result));
	stats_["lastResultOk"] = !NVSDK_NGX_FAILED(NVSDK_NGX_Result(result));
}

json Dlss::Stats() {
	json j = {{"built", true}, {"sdk", "310.9.16"}, {"available", available_.load()}, {"initTried", initTried_.load()}};
	std::lock_guard lock(statsMutex_);
	for (auto& [k, v] : stats_.items()) {
		j[k] = v;
	}
	json rr = rrStats_;
	rr["available"] = rrAvailable_.load();
	if (!rr.contains("created")) {
		rr["created"] = false;
	}
	j["rr"] = rr;
	return j;
}

#endif

} // namespace aether
