// Settings: one JSON file (AetherLoader/config/nec093.aether.graphics/aether.json). Every change goes through Settings::Set, which validates
// the value; the overlay and the MCP control tools use the same entry point.
#pragma once
#include "Common.h"
#include "Json.h"

namespace aether {

struct SettingsData {
	// overlay
	int overlayHotkey = 0x79;        // VK_F10
	bool overlayVisibleAtStart = false;
	float overlayScale = 1.0f;       // 0.5 .. 3
	float overlayTextScale = 1.0f;   // 1 .. 1.6: the text only, on top of overlayScale
	bool overlayHud = false;         // a small frame rate readout in the corner while the overlay is closed
	// free camera (its hotkey switches it on and off; the game's camera is overridden while it is on)
	int freeCameraHotkey = 0x7A;     // VK_F11
	float freeCameraSpeed = 4.0f;    // 0.2 .. 100 m/s (Shift x5, Ctrl x0.2)
	float freeCameraSensitivity = 0.08f; // 0.01 .. 1 degrees per mouse count
	// path tracing: off | freeCamera (while the free camera is on) | always
	std::string pathTracing = "freeCamera";
	int pathTracingBounces = 4;      // 1 .. 12
	int pathTracingMaxSamples = 4096; // 1 .. 100000 per pixel; the picture stays as it is after that
	std::string pathTracingDenoiser = "dlss_rr"; // dlss_rr (NVIDIA DLSS Ray Reconstruction where available) | off (the running average)
	int pathTracingSamplesPerFrame = 1; // 1 .. 4 paths per pixel each frame, averaged within the frame (with dlss_rr)
	// renderer
	std::string quality = "high";    // low | medium | high | ultra
	bool testTriangle = true;        // Phase 1 PoC pass
	bool gpuTiming = true;           // timestamp queries around the frame
	std::string debugView = "none";  // none | albedo | normal | roughness | metallic | depth | lighting | shadow | materialClass | sun | ambient | reflection
	// aether: Aether is the game's render pipeline (the game's own pipeline is never created). standalone: the same (older name).
	// hdrp: the game's own renderer, for reference pictures and measurements. compare: the game's renderer left, Aether right.
	std::string renderMode = "aether";
	bool shadows = true;             // directional shadow cascades
	bool ssao = true;                // screen space ambient occlusion
	bool antialiasing = true;        // temporal antialiasing
	// which temporal resolve runs while antialiasing is on: taa (Aether's own) | dlaa (NVIDIA DLSS at the full size) |
	// dlss_quality | dlss_balanced | dlss_performance | dlss_ultra_performance (DLSS super resolution: the scene is drawn
	// smaller and DLSS writes the full size). Without DLSS (not an NVIDIA RTX GPU, no nvngx_dlss.dll) it is TAA.
	std::string upscaler = "taa";
	bool bloom = true;               // bloom as the game's volumes set it
	bool motionBlur = true;          // motion blur as the game's volumes set it
	bool particles = true;           // the game's particle systems and tyre smoke
	bool ssr = true;                 // screen space reflections as the game's volumes set them
	bool volumetricFog = true;       // fog lit by the sun through the shadow map (light shafts), where the scene has fog
	bool followSceneNight = true;    // the physical sky turns to night when the game's scene does (its day / night switch)
	bool radianceCascades = true;    // the indirect light everywhere from radiance cascades (needs the ray tracing geometry); off: the probe grid
	bool rayTracedGi = false;        // the opaque surfaces' ambient light per pixel from long rays (with rayTracedLighting) instead of the cascades or the probes
	bool lampShadows = true;         // lamps (and head lamps) that cast shadows are shadowed by rays (needs the ray tracing geometry)
	bool rayTracedLighting = true;   // the sun's shadow and the ambient occlusion from rays (NRD SIGMA, REBLUR); off: shadow map and screen space occlusion
	bool lensFlare = true;           // ghosts, a halo and rays from the sun and from bright lamps, as a camera's lens makes them
	bool probeGi = true;             // indirect light from a grid of probes around the camera that trace rays (needs the ray tracing geometry; only when radianceCascades is off)
	bool reflectionProbe = true;     // the surroundings drawn into a cube around the camera every frame, for reflections
	bool rayTracedReflections = true; // smooth surfaces mirror what rays through the scene's triangles find (also off screen)
	bool indirectLight = true;       // light bounced off nearby surfaces (screen space, with the ambient occlusion pass)
	bool sky = true;                 // sky background and environment reflections
	// Aether's own graphics options (the overlay's Settings tab). The game's graphics menu is not read.
	std::string skyMode = "physical"; // physical: Aether's atmosphere, lit by the sun at timeOfDay | scene: the scene's own sky texture and sun
	float timeOfDay = 14.0f;          // hours, 0 .. 24 (physical sky)
	float sunHeading = 0.0f;          // degrees: turns the sun's path about the up axis (physical sky)
	float cloudCoverage = 0.4f;       // 0 .. 1: how much of the sky the clouds take (physical sky; 0 = none)
	float cloudSpeed = 1.0f;          // 0 .. 4: how fast the clouds drift
	float shadowSoftness = 1.5f; // How far the sun's shadow edges are blurred, in shadow map texels (0 = hard)
	float motionBlurStrength = 1.0f; // Motion blur strength, as a factor on what the scene asks for
	float aoStrength = 1.0f;        // 0 .. 2: ambient occlusion (1: as measured; above: darker than measured)
	float aoRadius = 1.5f;          // 0.2 .. 4 m: how far around a place the ambient occlusion looks
	float lensFlareStrength = 1.0f; // 0 .. 3: factor on the lens flare
	float bloomStrength = 0.3f; // Bloom strength, as a factor on what the scene asks for
	float indirectStrength = 1.0f; // 0 .. 3: factor on the bounced light
	// 0 .. 64: skinned meshes (cars, people) whose ray tracing structure is refitted to their current pose per frame, in
	// turn, on the GPU from Unity's skinned vertex buffer (0: skinned meshes are not in the ray structures at all; their
	// shadow comes from the untraced casters' shadow map). Their geometry is read back once only: reading the pose back
	// every frame made Unity's frame fence fall behind and frames were skipped (the picture flickered).
	float skinnedRayUpdates = 8.0f;
	float moonlight = 0.5f;        // 0 .. 4: the moon's light at night, as a factor (1: a night one can see the yard in; 0.25: nearly black)
	float fogStrength = 1.0f;      // 0 .. 20: factor on the fog density (physical sky: the air's own haze; else the scene's fog)
	float smokeOpacity = 1.0f; // Tyre smoke opacity factor
	float lodBias = 2.8f; // Unity's LOD bias: higher keeps detailed models up to a greater distance
	int anisotropicLevel = 8; // Anisotropic texture filtering level
	int vsync = 1; // Vertical sync: 0 off, 1 every refresh, 2 every second refresh
	std::string tonemap = "host";    // host (the curve of the game renderer) | agx | aces | neutral | none
	// The game's own tone mapping and colour grading: its tone curve, its post exposure and its shadows / midtones /
	// highlights grading, as its volumes set them. Off: none of them; tonemap "host" is drawn with AgX.
	bool hostGrading = true;
	std::string reflectionResolution = "full"; // full: a reflection ray for every pixel | half: one for each 2 x 2 block
	std::string reflectionDenoiser = "nrd"; // nrd | off: the reflection rays sample each surface's GGX lobe and NVIDIA NRD (REBLUR) denoises them (off: mirror rays, not denoised)
	std::string autoExposure = "always"; // scene: as the scene's exposure says | always: scenes with a fixed exposure are metered too (eye adaptation)
	float exposureEV = 0.0f;         // exposure bias in stops on top of the scene's exposure
	int vramBudgetMB = 2048;         // budget for Aether-owned GPU memory (0 = none): crossing it is reported, nothing is freed
	int vramWarnPercent = 80;        // warning threshold in percent of the budget
	// mcp
	std::string mcpControl = "ask";  // deny | ask | allow: what a control request from an AI agent needs
	int mcpApprovalTimeoutSec = 30;  // 5 .. 300
	// logging
	std::string logLevel = "info";   // debug | info | warning | error
};

class Settings {
public:
	static Settings& Get();
	void Load(const std::wstring& file);
	bool Save();
	SettingsData Snapshot();
	json ToJson();
	// Sets one value by its JSON name. Returns "" on success, else why the value was refused.
	std::string Set(const std::string& key, const json& value, const char* source);
	static json Schema(); // name -> {type, allowed / min / max, description}
	uint64_t Revision() const { return revision_.load(); }

private:
	std::mutex mutex_;
	SettingsData data_;
	std::wstring file_;
	std::atomic<uint64_t> revision_{1};
};

// Render features that exist as named switches (set_render_feature). Phase 1 has the PoC pass and GPU timing.
const std::vector<std::string>& RenderFeatureNames();
const std::vector<std::string>& QualityNames();
const std::vector<std::string>& RenderModeNames();
const std::vector<std::string>& DebugViewNames();

} // namespace aether
