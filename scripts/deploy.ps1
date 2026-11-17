# Installs the built files into the game (game closed), or removes them.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\deploy.ps1 [-Loader AetherLoader|BepInEx|Ksl] [-Config RelWithDebInfo] [-GameDir <dir>] [-Build] [-Remove]
#
# -Loader AetherLoader (default): an Aether mod. Aether Loader (BepInEx\plugins\AetherLoader, from the
#   CarX-Drifting-Aether-Loader repository) must be installed.
#   AetherLoader\mods\AetherGraphics\mod.json, AetherGraphics.dll         the mod (its manifest and code)
#   AetherLoader\mods\AetherGraphics\native\AetherGraphicsNative.dll      the native runtime (the mod copies it to <Game>_Data\Plugins\x86_64 on start)
#   AetherLoader\mods\AetherGraphics\native\dxcompiler.dll, dxil.dll      DXC for the DXR shaders (the Windows SDK's redistributables)
#   AetherLoader\mods\AetherGraphics\native\nvngx_dlss.dll                NVIDIA DLSS (when built with the SDK; NGX loads it from here)
#   AetherLoader\mods\AetherGraphics\native\nvngx_dlssd.dll               NVIDIA DLSS Ray Reconstruction (the path tracer's denoiser; likewise)
#   AetherLoader\mods\AetherGraphics\shaders\*.hlsl, hdrp\aether_hdrp_dx12.bundle
#   Settings: AetherLoader\config\nec093.aether.graphics. Logs, captures and benchmarks: the mod's folder.
#   A BepInEx install (BepInEx\plugins\AetherGraphics) is moved in: its logs, captures, benchmarks and settings are kept,
#   its plugin DLL removed (the mod must not start twice).
# -Loader BepInEx: a plain BepInEx plugin in BepInEx\plugins\AetherGraphics, settings BepInEx\config\AetherGraphics. The
#   Aether Loader copy's mod.json and DLL are removed (its data is kept).
# -Loader Ksl: kino\mods\AetherGraphics.ksm (signed by tools\maykr.exe with the build key, else the raw AetherGraphics.dll) and
#   kino\mods\AetherGraphics\ for the native runtime, shaders and bundle. Settings: kino\config\AetherGraphics.
# An install under the old name (Aether: plugins\Aether, config\Aether, AetherNative.dll) is moved to the new one.
# -Build builds everything first. -Remove deletes Aether Graphics' files for that loader (settings are kept).
# Nothing of the game's own files is changed.
param(
	[ValidateSet('AetherLoader', 'BepInEx', 'Ksl')][string]$Loader = 'AetherLoader',
	[string]$Config = 'RelWithDebInfo',
	[string]$GameDir = 'C:\Program Files (x86)\Steam\steamapps\common\CarX Drift Racing Online',
	[switch]$Build,
	[switch]$Remove
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (Get-Process | Where-Object { $_.ProcessName -like 'Drift Racing Online*' }) { Write-Host 'DEPLOY FAILED: the game is running (the DLLs are locked). Close it first.'; exit 1 }

$kslMods = Join-Path $GameDir 'kino\mods'
$bieData = Join-Path $GameDir 'BepInEx\plugins\AetherGraphics'
$bieConfig = Join-Path $GameDir 'BepInEx\config\AetherGraphics'
$alData = Join-Path $GameDir 'AetherLoader\mods\AetherGraphics'
$alConfig = Join-Path $GameDir 'AetherLoader\config\nec093.aether.graphics'
switch ($Loader) {
	'AetherLoader' {
		if (-not (Test-Path (Join-Path $GameDir 'BepInEx\plugins\AetherLoader\AetherLoader.dll'))) { Write-Host "DEPLOY FAILED: Aether Loader is not installed in $GameDir (CarX-Drifting-Aether-Loader: scripts\deploy.ps1 -Build)"; exit 1 }
		$modData = $alData; $configDir = $alConfig
	}
	'BepInEx' {
		if (-not (Test-Path (Join-Path $GameDir 'BepInEx\core\BepInEx.dll'))) { Write-Host "DEPLOY FAILED: BepInEx is not installed in $GameDir (docs/development/build.md)"; exit 1 }
		$modData = $bieData; $configDir = $bieConfig
	}
	'Ksl' {
		if (-not (Test-Path $kslMods)) { Write-Host "DEPLOY FAILED: $kslMods not found (is KSL installed? pass -GameDir)"; exit 1 }
		$modData = Join-Path $kslMods 'AetherGraphics'; $configDir = Join-Path $GameDir 'kino\config\AetherGraphics'
	}
}
$plugin = Get-ChildItem $GameDir -Directory -Filter '*_Data' | ForEach-Object { Join-Path $_.FullName 'Plugins\x86_64\AetherGraphicsNative.dll' } | Select-Object -First 1
$kslFiles = @((Join-Path $kslMods 'AetherGraphics.dll'), (Join-Path $kslMods 'AetherGraphics.ksm'))

# moves what a folder holds into another one that may exist already (files already there are kept)
function Merge-Folder([string]$from, [string]$to) {
	if (-not (Test-Path $from)) { return }
	New-Item -ItemType Directory -Force $to | Out-Null
	foreach ($item in Get-ChildItem $from -Force) {
		$target = Join-Path $to $item.Name
		if ($item.PSIsContainer) { Merge-Folder $item.FullName $target }
		elseif (-not (Test-Path $target)) { Move-Item $item.FullName $target }
	}
	Remove-Item -Recurse -Force $from
	Write-Host "moved $from into $to"
}

# the old name (Aether, until 0.3.0): move its folders to the new names, remove its DLLs
$oldConfigRoot = switch ($Loader) { 'Ksl' { Join-Path $GameDir 'kino\config' } default { Join-Path $GameDir 'BepInEx\config' } }
$oldData = if ($Loader -eq 'Ksl') { Join-Path $kslMods 'Aether' } else { Join-Path $GameDir 'BepInEx\plugins\Aether' }
$newOldData = if ($Loader -eq 'Ksl') { $modData } else { $bieData }
foreach ($pair in @(@($oldData, $newOldData), @((Join-Path $oldConfigRoot 'Aether'), (Join-Path $oldConfigRoot 'AetherGraphics')))) {
	if ((Test-Path $pair[0]) -and -not (Test-Path $pair[1])) { Move-Item $pair[0] $pair[1]; Write-Host "moved $($pair[0]) to $($pair[1])" }
}
$legacy = @((Join-Path $newOldData 'Aether.dll'), (Join-Path $newOldData 'native\AetherNative.dll'), (Join-Path $newOldData 'native\AetherNative.pdb'),
	(Join-Path $kslMods 'Aether.dll'), (Join-Path $kslMods 'Aether.ksm'))
$legacy += Get-ChildItem $GameDir -Directory -Filter '*_Data' | ForEach-Object { Join-Path $_.FullName 'Plugins\x86_64\AetherNative.dll' }
foreach ($f in $legacy) { if (Test-Path $f) { Remove-Item -Force $f; Write-Host "removed the old-name file $f" } }

if ($Remove) {
	$files = @($plugin)
	if ($Loader -eq 'Ksl') { $files += $kslFiles }
	foreach ($f in $files) { if ($f -and (Test-Path $f)) { Remove-Item -Force $f; Write-Host "removed $f" } }
	if (Test-Path $modData) { Remove-Item -Recurse -Force $modData; Write-Host "removed $modData" }
	Write-Host "Aether Graphics removed ($Loader; settings kept in $configDir)"
	exit 0
}

if ($Build) {
	& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build-native.ps1') -Config $Config
	if ($LASTEXITCODE -ne 0) { exit 1 }
	& dotnet build (Join-Path $repo 'src\AetherGraphics.Managed\AetherGraphics.Managed.csproj') -c Release -nologo -v q "-p:GameDir=$GameDir" "-p:Loader=$Loader"
	if ($LASTEXITCODE -ne 0) { Write-Host 'DEPLOY FAILED: the C# mod did not build'; exit 1 }
}

$native = Join-Path $repo "build\native\$Config\AetherGraphicsNative.dll"
$managedDir = Join-Path $repo "build\managed\$Loader\Release"
$managed = Join-Path $managedDir 'AetherGraphics.dll'
foreach ($f in $native, $managed) { if (-not (Test-Path $f)) { Write-Host "DEPLOY FAILED: $f not found (run with -Build)"; exit 1 } }

# one loader at a time: the other installs' code goes, their data comes along
if ($Loader -eq 'AetherLoader') {
	if (Test-Path $bieData) {
		foreach ($f in 'AetherGraphics.dll', 'AetherGraphics.pdb') { Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $bieData $f) }
		Merge-Folder $bieData $modData
	}
	if (Test-Path $bieConfig) { Merge-Folder $bieConfig $configDir }
} elseif ($Loader -eq 'BepInEx') {
	foreach ($f in 'AetherGraphics.dll', 'AetherGraphics.pdb', 'mod.json') {
		$p = Join-Path $alData $f
		if (Test-Path $p) { Remove-Item -Force $p; Write-Host "removed the Aether Loader copy $p (the mod must not start twice)" }
	}
}

$nativeDir = Join-Path $modData 'native' # a sub-folder: the loaders look at the .dll files next to the mod as possible mods
New-Item -ItemType Directory -Force (Join-Path $modData 'shaders'), $nativeDir | Out-Null
Copy-Item -Force $native $nativeDir
$pdb = [System.IO.Path]::ChangeExtension($native, '.pdb')
if (Test-Path $pdb) { Copy-Item -Force $pdb $nativeDir }
# DXC (copied beside the build by CMake): without it the DXR option traces with the shader walk
foreach ($dxc in 'dxcompiler.dll', 'dxil.dll') {
	$from = Join-Path (Split-Path $native) $dxc
	if (Test-Path $from) { Copy-Item -Force $from $nativeDir } else { Write-Host "note: $dxc not in the build folder: DXR shaders will not compile" }
}
# DLSS (copied beside the build by CMake when the SDK is in build\deps): NGX loads it from the mod's native folder
$dlssDll = Join-Path (Split-Path $native) 'nvngx_dlss.dll'
if (Test-Path $dlssDll) { Copy-Item -Force $dlssDll $nativeDir } else { Write-Host 'note: nvngx_dlss.dll not in the build folder: DLSS / DLAA are not available (scripts\fetch-deps.ps1)' }
$dlssdDll = Join-Path (Split-Path $native) 'nvngx_dlssd.dll'
if (Test-Path $dlssdDll) { Copy-Item -Force $dlssdDll $nativeDir } else { Write-Host 'note: nvngx_dlssd.dll not in the build folder: DLSS Ray Reconstruction is not available (scripts\fetch-deps.ps1)' }
Copy-Item -Force (Join-Path $repo 'shaders\*') (Join-Path $modData 'shaders')
# HDRP's compute shaders for DX12 (scripts\build-hdrp-bundle.ps1): without them the game's own renderer shows nothing on DX12
$bundle = Join-Path $repo 'build\unity\aether_hdrp_dx12.bundle'
if (Test-Path $bundle) {
	New-Item -ItemType Directory -Force (Join-Path $modData 'hdrp') | Out-Null
	Copy-Item -Force $bundle (Join-Path $modData 'hdrp')
} elseif (-not (Test-Path (Join-Path $modData 'hdrp\aether_hdrp_dx12.bundle'))) {
	Write-Host 'WARNING: build\unity\aether_hdrp_dx12.bundle is missing (scripts\build-hdrp-bundle.ps1): the game picture stays black on DX12'
}

if ($Loader -ne 'Ksl') {
	Copy-Item -Force $managed $modData
	if ($Loader -eq 'AetherLoader') {
		Copy-Item -Force (Join-Path $managedDir 'mod.json') $modData
		$managedPdb = Join-Path $managedDir 'AetherGraphics.pdb'
		if (Test-Path $managedPdb) { Copy-Item -Force $managedPdb $modData }
	}
	foreach ($f in $kslFiles) { if (Test-Path $f) { Remove-Item -Force $f; Write-Host "removed the KSL copy $f (the mod must not start twice)" } }
	Write-Host "mod: $modData\AetherGraphics.dll ($Loader)"
} else {
	$ksm = $kslFiles[1]
	$signed = (Test-Path $ksm) -and ((Get-Item $ksm).LastWriteTimeUtc -ge (Get-Item $managed).LastWriteTimeUtc)
	if ($signed) {
		Remove-Item -Force -ErrorAction SilentlyContinue $kslFiles[0] # one copy of the mod only
		Write-Host "mod: $ksm (signed)"
	} else {
		Remove-Item -Force -ErrorAction SilentlyContinue $ksm
		Copy-Item -Force $managed $kslMods
		Write-Host "mod: $kslMods\AetherGraphics.dll (RAW dll: KSL loads it only for accounts allowed to load raw dlls; build the .ksm with maykr and the build key)"
	}
}
Write-Host "native runtime and shaders: $modData; settings: $configDir"
Write-Host 'DEPLOY OK'
exit 0
