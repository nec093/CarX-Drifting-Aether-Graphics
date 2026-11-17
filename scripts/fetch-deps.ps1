# Downloads the libraries that are not committed into build\deps (the native build finds them there):
#   NRD v4.18.0 (NVIDIA Real-time Denoisers: the reflection denoiser), NVIDIA RTX SDKs License: object code only, so
#     its source stays out of the repository. Running this means accepting that license (build\deps\NRD-4.18.0\LICENSE.txt).
#   MathLib v11 and ShaderMake (NRD's build dependencies, MIT), at the versions NRD's CMakeLists.txt names.
#   DLSS v310.9.16 (NVIDIA NGX: DLSS super resolution and DLAA), NVIDIA RTX SDKs License: only the files the build and
#     the mod need (the NGX headers, nvsdk_ngx_s.lib, nvngx_dlss.dll, LICENSE.txt) into build\deps\DLSS-310.9.16;
#     nvngx_dlss.dll is installed next to the native runtime. Running this means accepting that license.
#     (nvsdk_ngx_s.lib: the static CRT's NGX library, as the native runtime is built with /MT. nvsdk_ngx_d.lib is the
#     dynamic CRT's and does not link into it.)
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\fetch-deps.ps1
#
# NRD's shaders are compiled with the Windows SDK's DXC (src\AetherGraphics.Native\CMakeLists.txt): nothing else is downloaded.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$deps = Join-Path $repo 'build\deps'
$downloads = Join-Path $deps 'downloads'
New-Item -ItemType Directory -Force $downloads | Out-Null
$items = @(
	@{ Name = 'NRD-4.18.0'; Url = 'https://github.com/NVIDIA-RTX/NRD/archive/refs/tags/v4.18.0.zip'; File = 'NRD-4.18.0.zip' },
	@{ Name = 'MathLib-11'; Url = 'https://github.com/NVIDIA-RTX/MathLib/archive/refs/tags/v11.zip'; File = 'MathLib-v11.zip' },
	@{ Name = 'ShaderMake-3f01623a76b092ac127d2debd79a672f55d77665'; Url = 'https://github.com/NVIDIA-RTX/ShaderMake/archive/3f01623a76b092ac127d2debd79a672f55d77665.zip'; File = 'ShaderMake.zip' }
)
foreach ($item in $items) {
	$target = Join-Path $deps $item.Name
	if (Test-Path $target) { Write-Host "have $($item.Name)"; continue }
	$zip = Join-Path $downloads $item.File
	Write-Host "downloading $($item.Url)"
	Invoke-WebRequest -UseBasicParsing -Uri $item.Url -OutFile $zip
	Expand-Archive -Path $zip -DestinationPath $deps -Force
	if (-not (Test-Path $target)) { Write-Host "FETCH FAILED: $($item.Name) not in the archive"; exit 1 }
}
# DLSS: single files (the SDK's repository is large: its archive is not needed)
$dlss = Join-Path $deps 'DLSS-310.9.16'
$dlssBase = 'https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16'
$dlssFiles = @('LICENSE.txt', 'include/nvsdk_ngx.h', 'include/nvsdk_ngx_defs.h', 'include/nvsdk_ngx_helpers.h', 'include/nvsdk_ngx_helpers_d3d.h', 'include/nvsdk_ngx_params.h', 'lib/Windows_x86_64/x64/nvsdk_ngx_s.lib', 'lib/Windows_x86_64/rel/nvngx_dlss.dll',
	# DLSS Ray Reconstruction (the path tracer's denoiser): its headers and nvngx_dlssd.dll
	'include/nvsdk_ngx_defs_dlssd.h', 'include/nvsdk_ngx_params_dlssd.h', 'include/nvsdk_ngx_helpers_dlssd.h', 'include/nvsdk_ngx_helpers_dlssd_d3d.h', 'lib/Windows_x86_64/rel/nvngx_dlssd.dll')
foreach ($f in $dlssFiles) {
	$out = Join-Path $dlss $f.Replace('/', '\')
	if (Test-Path $out) { continue }
	New-Item -ItemType Directory -Force (Split-Path -Parent $out) | Out-Null
	Write-Host "downloading $dlssBase/$f"
	Invoke-WebRequest -UseBasicParsing -Uri "$dlssBase/$f" -OutFile $out
}
Write-Host "FETCH OK: $deps"
