// Aether's scene renderer: a clustered forward ("forward+") pipeline over the scene store's data.
//   ShadowCascades -> DepthPrepass -> (light clustering on the CPU) -> Opaque -> Sky -> Transparent -> Composite
// Everything is recorded into the one command list the caller hands in; the passes are declared in the render graph.
// docs/architecture/renderer.md. Render thread only, except Stats().
#pragma once
#include <atomic>
#include <chrono>
#include "../Core/Common.h"
#include "../Core/Config.h"
#include "../Core/Json.h"
#include "../Core/Math.h"
#include "../Device/Host.h"
#include "../Profiler/Profiler.h"
#include "../Scene/SceneStore.h"
#include "GpuTimer.h"
#include "RenderGraph.h"

#include <map>

namespace aether {

struct MeshLodSet; // MeshLod.h

class SceneRenderer {
public:
	static SceneRenderer& Get();

	struct Target {
		ID3D12Resource* backBuffer = nullptr;       // in RENDER_TARGET, stays there
		D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv = {};
		DXGI_FORMAT backBufferFormat = DXGI_FORMAT_UNKNOWN;
		uint32_t width = 0, height = 0;
	};

	// Applies the pending scene changes and, when `draw` is set and the scene has a camera, records the frame.
	// `retireFence` is the fence value this frame's list will be submitted with.
	// Returns true when the back buffer was written.
	bool Render(const HostGfx& host, ID3D12GraphicsCommandList* list, const Target& target, bool draw, UINT64 retireFence, uint64_t frame, const SettingsData& settings);
	// After the list was submitted.
	void EndFrame(const HostGfx& host, UINT64 fence);
	void Release(bool leak);

	// ---- any thread
	json Stats();
	float CurrentHours() const { return currentHours_.load(); } // the time of day the sky is drawn at (any thread)
	int CurrentDay() const { return currentDay_.load(); }
	// the environment as the last frame had it (any thread): how wet the ground is (0 .. 1) and the puddles on it, and
	// the wind near the ground (m/s, the way it blows; the tyre smoke drifts with it) and its gustiness
	float Wetness() const { return wetnessShown_.load(); }
	float Puddles() const { return puddlesShown_.load(); }
	Vec3 GroundWind() const { return {groundWind_[0].load(), groundWind_[1].load(), groundWind_[2].load()}; }
	float Gustiness() const { return gustShown_.load(); }
	void SetSceneNight(bool night) { sceneNight_.store(night); }
	// (towards the sun or the moon, as the last frame had it; read without a lock: a torn read is three old or new floats)
	Vec3 SunDirection() const { return sunDirection_; }
	// the path traced picture (setting pathTracing): running, and the samples per pixel averaged so far
	// (denoised: by DLSS Ray Reconstruction, a fresh sample every frame; samples then counts the frames since its last reset)
	struct PathTraceStatus { bool active = false; uint32_t samples = 0; bool denoised = false; };
	PathTraceStatus PathTracing() const { return {ptActiveShown_.load(), ptSamplesShown_.load(), ptRrShown_.load()}; }
	json GraphDescription() { return graph_.Describe(); }

private:
	enum PassKind { kDepth = 0, kDepthAlpha, kShadow, kShadowAlpha, kForward, kForwardBlend, kForwardAdditive, kDecal, kForwardProbe, kPassKindCount };
	enum Cull { kCullBack = 0, kCullFront, kCullNone };
	struct DrawItem {
		const SceneMesh* mesh;
		const SceneMaterial* material;
		uint32_t subMesh;
		uint32_t instanceIndex;
		uint32_t materialIndex;
		uint32_t cull;      // Cull
		float depth;        // view depth of the bounds' centre
		uint64_t sortKey;
		Vec3 center;        // world space bounds
		float radius;
		bool castsShadow;
		bool mirrored;
		// automatic level of detail (MeshLod.h): the mesh's levels (null: none), the level picked for the camera, and
		// the instance's scale (the levels' errors are in the mesh's own units)
		const MeshLodSet* lods = nullptr;
		uint32_t lod = 0;
		float lodScale = 1.0f;
	};
	struct Cascade { Mat4 viewProj; Mat4 atlasMatrix; float splitFar; float texelWorld; Vec3 center; float radius; D3D12_VIEWPORT viewport; };

	bool EnsureObjects(const HostGfx& host, uint64_t frame);
	bool EnsureShadowAtlas(uint32_t cascadeSize, uint64_t frame, UINT64 retireFence);
	void UploadDefaults(ID3D12GraphicsCommandList* list);
	ID3D12PipelineState* Pipeline(uint32_t layoutId, PassKind kind, uint32_t cull);
	// full screen passes: one pixel shader each (scene_post.hlsl), named in kFullscreenEntry (SceneRenderer.cpp)
	enum FullscreenKind { kFullscreenSky = 0, kFullscreenComposite, kFullscreenAo, kFullscreenAoBlur, kFullscreenExposure, kFullscreenIrradiance, kFullscreenTaa, kFullscreenMotionBlur, kFullscreenBloomDown, kFullscreenBloomUp, kFullscreenSsr, kFullscreenTransmittanceLut, kFullscreenMultiScatterLut, kFullscreenSkyView, kFullscreenCloudNoise, kFullscreenSkyClouds, kFullscreenClouds, kFullscreenFog, kFullscreenFogApply, kFullscreenReflectionBlur, kFullscreenBvhDebug, kFullscreenRayTrace, kFullscreenGiRays, kFullscreenGiIrradiance, kFullscreenGiDepth, kFullscreenLensFlare, kFullscreenSmokeApply, kFullscreenSunRays, kFullscreenAoRays, kFullscreenRcTrace, kFullscreenRcIrradiance, kFullscreenRcDepth, kFullscreenPathTrace, kFullscreenPathTraceApply, kFullscreenDlssMotion, kFullscreenPathTraceRr, kFullscreenRain, kFullscreenKindCount };
	// draws a full screen pass into `output` (its whole size) with up to three inputs; `params` reach the shader as g_PassParams
	bool Fullscreen(RenderGraph::PassContext& ctx, FullscreenKind kind, GpuTexture* output, GpuTexture* in0, GpuTexture* in1 = nullptr, GpuTexture* in2 = nullptr, const float* params = nullptr, GpuTexture* in3 = nullptr);
	// The same with several render targets (the pipeline is made for their formats, in order; no blending).
	bool FullscreenTargets(RenderGraph::PassContext& ctx, FullscreenKind kind, GpuTexture* const* outputs, uint32_t outputCount, GpuTexture* in0, GpuTexture* in1, GpuTexture* in2, const float* params);
	uint32_t postNext_ = 0; // the next free table of this frame's post tables
	std::shared_ptr<GpuTexture> irradiance_; // the sky's diffuse light per direction (octahedral map)
	// temporal antialiasing: the resolved picture of the frame before, and the camera it was drawn with
	std::shared_ptr<GpuTexture> history_[2];
	uint32_t historyIndex_ = 0, historyCamera_ = 0;
	bool historyValid_ = false;
	Mat4 prevViewProj_ = Mat4::Identity();
	// the reflection denoiser (NRD): the camera without jitter, this frame and the one before, and what the passes
	// reconstruct positions with
	Mat4 nrdView_ = Mat4::Identity(), nrdProj_ = Mat4::Identity(), nrdViewPrev_ = Mat4::Identity(), nrdProjPrev_ = Mat4::Identity(), invViewProj_ = Mat4::Identity();
	bool rtGi_ = false;      // this frame: the ambient light per pixel from long rays (setting rayTracedGi; the probe grid stops)
	// shadow cascades drawn in turns (setting shadowCaching): what was drawn last, and which are drawn this frame
	std::vector<Cascade> shadowCache_;
	std::vector<bool> cascadeDraw_;
	Vec3 cachedSunDirection_ = {0, 0, 0};
	uint32_t cachedCascadeSize_ = 0;
	const void* cachedAtlas_ = nullptr;
	bool naturalTiling_ = false; // this frame: large static meshes get hex tiling (setting naturalTiling)
	bool lampShadows_ = false; // this frame: lamps that cast shadows are shadowed by rays (setting lampShadows)
	bool rtLighting_ = false; // this frame: the sun's shadow and the ambient occlusion from rays (setting rayTracedLighting, LightingDenoiser)
	bool nrdFrame_ = false; // this frame: the reflection rays sample the surface's lobe and NRD denoises them (setting reflectionDenoiser = nrd)
	bool taa_ = false; // this frame has a temporal resolve: Aether's TAA, or DLSS (dlssMode_)
	// The temporal resolve by DLSS (setting upscaler; Dlss.h): DLAA at the full size, or super resolution, where
	// everything up to the resolve is drawn at the render size and DLSS writes the output size (the target's). Without
	// DLSS the render size is the output size. dlssBroken_: an evaluation failed for that setting: TAA until it changes.
	int dlssMode_ = -1; // Dlss::Mode
	uint32_t renderWidth_ = 0, renderHeight_ = 0;
	float dlssJitter_[2] = {0, 0}; // this frame's projection offset in render pixels (x right, y down)
	float mipBias_ = 0.0f;
	bool dlssBroken_ = false;
	int resolveByDlss_ = -1; // the frame before's resolve: 1 DLSS, 0 TAA, -1 none
	std::string dlssBrokenFor_, upscalerNote_;
	std::chrono::steady_clock::time_point dlssClock_ = std::chrono::steady_clock::now();
	float bloomMix_ = 0, bloomScatter_ = 0, motionBlur_ = 0;
	bool ownExposure_ = false;
	bool ssr_ = false; // screen space reflections this frame (the host has them on, and Aether's switch)
	// The physical sky (setting skyMode = physical): Aether's own atmosphere instead of the scene's sky texture, lit by
	// a sun whose place follows the time of day. The first two LUTs are made once, the sky view every frame.
	bool physicalSky_ = false;
	bool atmosphereLutsValid_ = false;
	float atmosphereScale_ = 1.0f; // the atmosphere's unit (sun illuminance 1 above the air) in scene units
	std::atomic<bool> sceneNight_{false}; // the game's own day / night switch is on night
	float moonStrength_ = 0.0f;    // the atmosphere's unit while the moon is the light (scene units)
	float sceneSunLum_ = 0.0f;     // the scene's own brightest directional light (its sun by day, its moon at night)
	float moonShare_ = 0.0f;       // 0: the light in the sky is the sun; up to 1: it is the moon (night)
	std::shared_ptr<GpuTexture> transmittanceLut_, multiScatterLut_, skyViewLut_;
	// clouds (physical sky): the noise their shape comes from (made once), and the sky with the clouds in it, which
	// is what reflections and the sky's diffuse light read
	std::shared_ptr<GpuTexture> cloudNoise_, skyCloudsLut_;
	bool clouds_ = false;
	bool volumetricFog_ = false; // this frame: the fog is marched through the shadow map instead of computed per surface
	// Reflection probes: the still surroundings drawn into cubes that stay where they are in the world (a grid of
	// places around the camera), a face or two per frame, kept while the camera is near. Every cube is six tiles in
	// one atlas, with its depth (where a mirrored ray from a surface ends) and a blurred copy for rough surfaces.
	// No vehicles and nothing that moves in them; the sky is not in them either (alpha 0 = look the sky up).
	bool reflection_ = false;
	struct LightProbe {
		int cell[3] = {0, 0, 0};
		Vec3 position = {0, 0, 0};
		uint32_t facesDone = 0;  // bit per face drawn since the probe took this place (0x3F: usable)
		uint32_t nextFace = 0;
		uint64_t lastWanted = 0; // the last frame the camera was near
		uint64_t lastDrawn = 0;
		uint64_t wholeSince = 0; // the frame its last missing face was drawn (0: not whole yet): it fades in from there
		bool used = false;
	};
	static const uint32_t kProbeSlots = 8;
	LightProbe lightProbes_[kProbeSlots];
	Vec3 probeCameraBefore_ = {0, 0, 0};
	Vec3 probeAhead_ = {0, 0, 0}; // where the camera is heading (its smoothed travel over about half a second)
	bool probeCameraValid_ = false;
	struct ProbeFaceJob { uint32_t slot = 0, face = 0; D3D12_GPU_VIRTUAL_ADDRESS cb = 0; float planes[6][4] = {}; };
	ProbeFaceJob probeJobs_[2];
	uint32_t probeJobCount_ = 0;
	void UpdateProbes(const SceneFrame& scene, uint64_t frame);
	std::shared_ptr<GpuTexture> reflectionAtlas_[2], reflectionDepth_; // [0] the cubes, [1] blurred
	std::vector<DrawItem> probeItems_;
	const float (*cullPlanes_)[4] = nullptr; // DrawItems: only what these planes let through
	uint32_t probeGlobalsTable_ = UINT32_MAX;
	// probe grid GI: this frame's rays, the probes' irradiance and their distances (each probe a tile of 8 x 8)
	std::shared_ptr<GpuTexture> giRays_, giIrradiance_, giDepth_;
	bool gi_ = false, giHistory_ = false;
	// radiance cascades (setting radianceCascades): the atlases of this frame and the frame before (in turn), level
	// 0's light per normal, and the camera position the atlas of the frame before was made for
	bool rc_ = false, rcHistory_ = false;
	std::shared_ptr<GpuTexture> rcAtlas_[2], rcIrradiance_, rcDepth_; // (rcDepth_: how far each level 0 probe sees, 8 x 8 directions)
	uint32_t rcIndex_ = 0;
	// path tracing (setting pathTracing): the running average of the samples (two in turn: read the one, write the
	// other), how many it holds, and what it was made for (when that changes, it starts anew)
	bool pathTrace_ = false;
	std::shared_ptr<GpuTexture> ptAccum_[2];
	uint32_t ptIndex_ = 0, ptSamples_ = 0;
	int ptResetBy_ = -1; // (which part of the key last started the average anew: for the pass's note)
	float ptKey_[40] = {};
	std::atomic<bool> ptActiveShown_{false};
	std::atomic<uint32_t> ptSamplesShown_{0};
	// the path tracer's denoiser (setting pathTracingDenoiser: dlss_rr): NVIDIA DLSS Ray Reconstruction takes a fresh
	// sample per pixel and its guides every frame instead of the running average. ptRr_: this frame's; ptRrOut_: its
	// output (a UAV); ptRrBroken_: an evaluation failed: the running average until the setting changes. Its history is
	// reset on a camera cut, a settings change or a size change, not when the camera moves.
	bool ptRr_ = false, ptRrBroken_ = false, ptRrReset_ = true, ptRrFailed_ = false;
	uint64_t ptRrBrokenRevision_ = 0;
	std::shared_ptr<GpuTexture> ptRrOut_;
	uint32_t ptRrCamera_ = 0, ptRrWidth_ = 0, ptRrHeight_ = 0;
	uint64_t ptRrRevision_ = 0;
	float ptRrPosition_[3] = {0, 0, 0};
	std::string ptDenoiserNote_;
	std::atomic<bool> ptRrShown_{false};
	float rcCamera_[3] = {0, 0, 0};
	bool lensFlare_ = false;
	float lensFlareStrength_ = 0.0f;
	int giMin_[3] = {0, 0, 0}; // the grid cell of the volume's lowest corner
	bool bvhReady_ = false;      // this frame: rays can be traced through the scene's triangles (Bvh.h)
	bool raysWanted_ = false;    // this frame: something traces rays (reflections, a debug view)
	bool rayReflections_ = false; // this frame: smooth surfaces mirror what rays find
	bool raysSupported_ = false;  // the device has DXR 1.1 (inline ray tracing): the shaders were built with rays
	std::vector<uint32_t> rayMaterials_; // per ray instance and sub mesh, two numbers: its index in this frame's material buffer, its base texture's place in the SRV heap (UINT32_MAX: none)
	D3D12_GPU_VIRTUAL_ADDRESS rayMaterialsVa_ = 0;
	bool indirect_ = false;      // this frame: the occlusion pass also gathers light bounced off what it finds
	float cloudDrift_ = 0.0f, cloudDriftZ_ = 0.0f; // how far the wind has carried the clouds (noise uv, x and z)
	// the time of day as it flows (setting timeFlow): hours moved on since timeOfDay last changed
	std::chrono::steady_clock::time_point timeClock_ = std::chrono::steady_clock::now();
	float timeFlowHours_ = 0.0f, timeFlowFrom_ = -1.0f;
	std::atomic<float> currentHours_{12.0f};
	std::atomic<int> currentDay_{172};
	// The environment (docs/architecture/rendering-features.md, "風" and "天気"): one wind and one weather, eased from the
	// settings every frame (UpdateEnvironment), that the clouds, the plants, the smoke, the fog, the ground and the rain
	// all read. The weather's parts ease towards the chosen weather's numbers over about 20 s; the ground gets wet in a
	// minute or two of rain and dries over about ten minutes after it.
	struct Environment {
		float windSpeed = 3.0f;          // m/s (10 m above the ground), eased towards the setting
		float windX = 0.0f, windZ = 1.0f; // the way the wind blows (unit, world xz)
		float gust = 0.4f;
		double windTime = 0.0;           // s: the sway's and the gusts' clock (wraps every 4096 s)
		float dt = 0.0f;                 // the frame's step (s), for the sway of the frame before (motion vectors)
		float coverage = 0.0f;           // the weather's cloud coverage (the setting stays a minimum)
		float fogExtra = 0.0f;           // per metre: fog the weather adds to the air's haze or the scene's fog
		float fogShare = 0.0f;           // 0 .. 1: how much of the fog is the weather's low fog (a lower scale height)
		float gloom = 0.0f;              // 0 .. 1: how much a thick deck darkens the clouds' light (overcast, rain)
		float rain = 0.0f;               // 0 .. 1: how hard it rains now (eased in and out with the weather)
		float wetness = 0.0f;            // 0 .. 1: the ground
		float puddles = 0.0f;            // 0 .. 1: how much of the level ground stands under water
		float blend = 1.0f;              // 0 .. 1: how far the transition to the chosen weather has gone (stats)
		std::string weather = "clear";   // the weather the state eases towards
		std::string from = "clear";      // ... and the one it came from
	};
	Environment env_;
	std::chrono::steady_clock::time_point envClock_ = std::chrono::steady_clock::now();
	bool envStarted_ = false;
	std::atomic<float> wetnessShown_{0.0f}, puddlesShown_{0.0f}, gustShown_{0.4f};
	std::atomic<float> groundWind_[3] = {0.0f, 0.0f, 0.0f};
	void UpdateEnvironment(const SettingsData& settings);
	bool rain_ = false; // this frame: the rain pass draws streaks
	// automatic exposure: the value of the frame before (1 x 1), which this frame's metering moves from
	std::shared_ptr<GpuTexture> exposureHistory_[2];
	uint32_t exposureIndex_ = 0;
	bool exposureValid_ = false;
	std::chrono::steady_clock::time_point exposureTime_{};
	bool frameSky_ = false; // this frame has the host's sky cube
	uint32_t probesTable_ = UINT32_MAX, probesInFrame_ = 0;
	// instancing: every draw covers a run of this frame's instance map (indices into the instance buffer)
	uint32_t* instanceMap_ = nullptr;
	uint32_t instanceMapUsed_ = 0, instanceMapCapacity_ = 0, itemsDrawn_ = 0, itemsInstanced_ = 0;
	D3D12_GPU_VIRTUAL_ADDRESS instanceMapVa_ = 0;
	// automatic levels of detail (setting autoLod, MeshLod.h): this frame's switch, the error a level may show (pixels,
	// 2 / lodBias), and what the camera's view drew: instances and triangles per level, and the triangles the meshes
	// in full would have had
	bool autoLod_ = false;
	float lodPixelError_ = 0.7f;
	float lodMinDistance_ = 25.0f; // m: the nearest the first simpler level may be used (doubling per level)
	uint32_t lodInstances_[4] = {}, lodInstancesWithout_ = 0;
	uint64_t lodTriangles_[4] = {}, lodFullTriangles_ = 0;
	ID3D12PipelineState* FullscreenPipeline(FullscreenKind kind, DXGI_FORMAT format);
	bool aoActive_ = false;
	void BuildDrawLists(const SceneFrame& scene, uint64_t frame);
	void BuildCascades(const SceneFrame& scene, const SettingsData& settings);
	void BuildClusters(const SceneFrame& scene);
	bool UploadFrameData(const SceneFrame& scene, const Target& target, const SettingsData& settings);
	void BindCommon(ID3D12GraphicsCommandList* list, bool globals);
	void DrawItems(RenderGraph::PassContext& ctx, const std::vector<DrawItem>& items, PassKind opaqueKind, PassKind alphaKind, int cascade);

	ID3D12Device* device_ = nullptr;
	bool objectsReady_ = false;
	bool failedShaders_ = false;
	uint64_t shaderGeneration_ = 0;
	ComPtr<ID3D12RootSignature> root_;
	ComPtr<ID3DBlob> vsMain_, vsShadow_, psForward_, psPrepass_, psDepthAlpha_, psShadowAlpha_, vsFullscreen_, psDecal_, vsParticle_, psParticle_;
	// particles: this frame's quads (sorted, uploaded as a buffer the vertex shader reads) and their batches
	ComPtr<ID3D12PipelineState> particlePipeline_, smokePipeline_; // (smoke: into its own target at half size)
	bool particlePipelineFailed_ = false;
	std::vector<AeParticle> particleData_;
	std::vector<AeParticleBatch> particleBatches_;
	D3D12_GPU_VIRTUAL_ADDRESS particlesVa_ = 0;
	uint32_t particlesDrawn_ = 0;
	ID3D12PipelineState* ParticlePipeline(bool smokeTarget = false);
	ComPtr<ID3DBlob> psFullscreen_[kFullscreenKindCount];
	std::map<uint64_t, ComPtr<ID3D12PipelineState>> pipelines_;
	std::map<uint32_t, ComPtr<ID3D12PipelineState>> fullscreenPipelines_;
	ComPtr<ID3D12Resource> defaultsVb_;
	uint64_t defaultsVbId_ = 0;
	D3D12_VERTEX_BUFFER_VIEW defaultsView_ = {};
	std::shared_ptr<GpuTexture> white_, flatNormal_, black_;
	bool defaultsUploaded_ = false;
	std::shared_ptr<GpuTexture> shadowAtlas_;
	// with ray traced shadows: what the rays cannot find (meshes not in the ray structures: skinned meshes with
	// skinnedRayUpdates = 0, meshes whose triangles have not arrived yet) casts its shadow from a shadow map of its own,
	// half the atlas's size, the same cascades
	std::shared_ptr<GpuTexture> untracedAtlas_;
	bool untracedShadow_ = false;
	uint32_t rayMaterialPadded_ = 0; // this frame: ray instances whose mesh has more sub meshes than the renderer has materials
	uint32_t shadowCascadeSize_ = 0;
	uint32_t nullCubeSrv_ = UINT32_MAX, nullTextureSrv_ = UINT32_MAX, fallbackTable_ = UINT32_MAX;

	RenderGraph graph_;
	GpuTimer timer_;
	std::vector<std::string> timedPasses_;
	std::vector<uint32_t> passStampBegin_, passStampEnd_;

	// per frame
	std::vector<DrawItem> opaque_, transparent_, casters_, untracedCasters_, decals_; // decals_: boxes that darken what lies inside them (blob shadows)
	std::vector<uint8_t> instanceData_, materialData_;
	uint32_t instanceCount_ = 0, materialCount_ = 0;
	std::vector<Cascade> cascades_;
	std::vector<uint32_t> clusterGrid_, lightIndices_;
	std::vector<uint8_t> lightData_;
	uint32_t punctualCount_ = 0;
	Vec3 sunDirection_ = {0, 1, 0};
	Vec3 sunColor_ = {0, 0, 0};
	bool hasSun_ = false, sunShadows_ = false;
	uint32_t sunLayerMask_ = 0xFFFFFFFFu;
	D3D12_GPU_VIRTUAL_ADDRESS frameCb_ = 0, instancesVa_ = 0, materialsVa_ = 0, lightsVa_ = 0, gridVa_ = 0, indicesVa_ = 0;
	uint32_t globalsTable_ = UINT32_MAX, postTable_ = UINT32_MAX;
	uint64_t frame_ = 0;
	UINT64 retireFence_ = 0;

	std::mutex statsMutex_;
	json stats_ = json::object();
};

} // namespace aether
