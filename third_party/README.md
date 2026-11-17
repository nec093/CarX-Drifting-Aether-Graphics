# third_party

| ライブラリ | 版 | ライセンス | 用途 |
|---|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.93.0（無改変。本体と `backends/imgui_impl_dx12`、`imgui_impl_win32`） | MIT（`imgui/LICENSE.txt`） | ゲーム内オーバーレイ |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0（`single_include` の `json.hpp`） | MIT（`nlohmann/LICENSE.MIT`） | ログ、設定、IPC の JSON |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | 1.3（無改変。`src/simplifier.cpp`、`src/allocator.cpp`、`src/meshoptimizer.h` だけ） | MIT（`meshoptimizer/LICENSE.md`） | 自動 LOD（静的メッシュの簡略化した索引、`Renderer/MeshLod.cpp`） |
| [Intel XeGTAO](https://github.com/GameTechDev/XeGTAO) | 主パスを HLSL へ移植（`shaders/scene_post.hlsl` の `PSAmbientOcclusion`。詳細は `XeGTAO/README.md`） | MIT（`XeGTAO/LICENSE`） | アンビエントオクルージョン |

| [MathLib](https://github.com/NVIDIA-RTX/MathLib) | v11 の `ml.hlsli`（無改変、`shaders/ml.hlsli` として配置） | MIT（`MathLib/LICENSE.txt`） | 反射の光線の GGX ローブのサンプリング（VNDF） |

**NRD（NVIDIA Real-time Denoisers）v4.18.0** は、反射のノイズ除去に使います。NVIDIA RTX SDKs License で、配布はオブジェクトコードとしてだけ認められるため、リポジトリには入れません。`scripts\fetch-deps.ps1` が NRD と、その依存の MathLib・ShaderMake を `build\deps\` に取得します（このスクリプトを実行することは、NRD のライセンスに同意することです）。NRD のシェーダーは Windows SDK の DXC でコンパイルし、`AetherNative.dll` に埋め込みます。Aether の `Renderer/NrdPasses.hlsl` も `NRD.hlsli` を使うので、同じくビルド時にコンパイルして埋め込みます。`build\deps\` に NRD が無ければ、ノイズ除去なしでビルドされます。

**DLSS（NVIDIA NGX）SDK v310.9.16** は、シーンの時間方向の解決（DLAA と DLSS 超解像）に使います。NRD と同じく NVIDIA RTX SDKs License で、配布はオブジェクトコードとしてだけ認められるため、リポジトリには入れません。`scripts\fetch-deps.ps1` が、ビルドと実行に要るファイルだけ（NGX のヘッダー、静的 CRT 用の `nvsdk_ngx_s.lib`、`nvngx_dlss.dll`、`LICENSE.txt`）を `build\deps\DLSS-310.9.16\` に取得します（このスクリプトを実行することは、DLSS のライセンスに同意することです）。ネイティブランタイムは静的 CRT（`/MT`）でビルドするので、動的 CRT 用の `nvsdk_ngx_d.lib` はリンクできません。`nvngx_dlss.dll` はビルド時にビルド出力へコピーし、`scripts\deploy.ps1` がネイティブランタイムの隣（Mod の `native` フォルダー）に入れます。`build\deps\` に DLSS が無ければ DLSS なしでビルドされ、設定 `upscaler` は Aether の TAA のままです。
同じ SDK の **DLSS Ray Reconstruction**（DLSS-D）を、パストレーシングのノイズ除去（設定 `pathTracingDenoiser`）に使います。ライセンスは同じ NVIDIA RTX SDKs License で、リポジトリには入れません。`scripts\fetch-deps.ps1` が、そのヘッダー（`nvsdk_ngx_defs_dlssd.h`、`nvsdk_ngx_params_dlssd.h`、`nvsdk_ngx_helpers_dlssd.h`、`nvsdk_ngx_helpers_dlssd_d3d.h`）と `nvngx_dlssd.dll`（RR のモデル）を同じフォルダーに取得します。`nvngx_dlssd.dll` も `nvngx_dlss.dll` と同じく、ビルド時にビルド出力へコピーし、`scripts\deploy.ps1` が Mod の `native` フォルダーに入れます。DLL が無いときは RR を使えないと報告し、パストレーシングはサンプルの平均のままです。

DXC（`dxcompiler.dll`、`dxil.dll`）はここに置かず、ビルド時に Windows SDK の再配布用フォルダー（`Redist\D3D\x64`、CMake の `AETHER_DXC_DIR`）からビルド出力へコピーします。SM 6.5 のシェーダー（DXR）を実行時にコンパイルするために使います。

Unity Native Plugin API のヘッダー（`IUnityGraphicsD3D12.h` ほか、Unity Companion License）はここに置かず、Unity エディターの `Editor\Data\PluginAPI` を参照します。
