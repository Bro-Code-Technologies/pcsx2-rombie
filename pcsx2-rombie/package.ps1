# Builds the engine's download: a zip with the engine, the PCSX2 runtime files it loads, the MSVC
# runtime, the licenses and a README. Run from the PCSX2 root after a Release AVX2 x64 build.
#   powershell -File pcsx2-rombie\package.ps1 [-Bin bin] [-Out dist]
param(
	[string]$Bin = "bin",
	[string]$Out = "dist"
)
$ErrorActionPreference = 'Stop'

$version = (Select-String -Path pcsx2-rombie\Version.h -Pattern '#define ROMBIE_ENGINE_VERSION "(.+)"').Matches[0].Groups[1].Value
$name = "rombie-ps2-engine-$version-windows-x64"
$stage = Join-Path $Out $name
$zip = "$stage.zip"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
if (Test-Path $zip) { Remove-Item -Force $zip }
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item "$Bin\pcsx2-rombiex64-avx2.exe" "$stage\rombie-ps2-engine.exe"

# What the engine imports (dumpbin /dependents, recursively), plus what PCSX2 loads at runtime:
# shaderc for the Vulkan renderer, HarfBuzz for FreeType, and the D3D12 Agility SDK.
$dlls = 'dxcompiler', 'freetype', 'harfbuzz', 'jpeg62', 'libpng16', 'libsharpyuv', 'libwebp', 'lz4',
	'plutosvg', 'plutovg', 'ryml', 'SDL3', 'shaderc_shared', 'z', 'zstd'
foreach ($dll in $dlls) { Copy-Item "$Bin\$dll.dll" $stage }
Copy-Item -Recurse "$Bin\D3D12" "$stage\D3D12"
Copy-Item -Recurse "$Bin\resources" "$stage\resources"

# The MSVC runtime, app-local, so nobody has to install the Visual C++ Redistributable first. Visual
# Studio's own redist for its toolset (VC\Auxiliary\Build\Microsoft.VCRedistVersion.default.txt).
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -version "[17,18)" -latest -property installationPath
$redistVersion = (Get-Content "$vs\VC\Auxiliary\Build\Microsoft.VCRedistVersion.default.txt").Trim()
$crt = "$vs\VC\Redist\MSVC\$redistVersion\x64\Microsoft.VC143.CRT"
foreach ($dll in 'msvcp140', 'vcruntime140', 'vcruntime140_1') { Copy-Item "$crt\$dll.dll" $stage }

New-Item -ItemType Directory "$stage\licenses" | Out-Null
Copy-Item COPYING.GPLv3 "$stage\LICENSE.txt"
Copy-Item "$Bin\docs\ThirdPartyLicenses.html" "$stage\licenses\"
# Microsoft's terms for D3D12Core.dll (LICENSE.txt from the Agility SDK 1.619.5 NuGet package the
# dependency script pins), and SDL3's license, which PCSX2's ThirdPartyLicenses.html leaves out. The
# MSVC runtime's terms are Visual Studio's, named in the README.
Copy-Item pcsx2-rombie\licenses\DirectX-Agility-SDK.txt "$stage\licenses\"
Copy-Item deps\licenses\SDL3\LICENSE.txt "$stage\licenses\SDL3.txt"

$readme = @"
Rombie PS2 Engine $version

Plays PS2 games in Rombie (https://rombie.app). It runs PCSX2 on this computer, without a
window of its own, and shows the game inside Rombie.

Setting it up
  1. Unzip this folder somewhere it can stay, for example Documents\Rombie PS2 Engine.
  2. Run rombie-ps2-engine.exe once. It says it's set up, and that's all.
  From then on Rombie starts the engine whenever you play a PS2 game. The first time, your
  browser asks whether to open "Rombie PS2 Engine": allow it, and tick "always allow" so it
  doesn't ask again.

Moving or removing it
  Moved the folder? Run rombie-ps2-engine.exe once more from its new place.
  To remove it, run "rombie-ps2-engine.exe --unregister", then delete the folder.

Privacy
  The engine only accepts connections from this computer, and only from Rombie. Your discs and
  BIOS stay on this computer: Rombie reads them from your ROM folder and hands them to the engine
  piece by piece. The engine doesn't save them anywhere.

Requirements
  64-bit Windows 10 or 11, and a processor with AVX2.

License
  The engine is free software under the GNU General Public License v3 (LICENSE.txt). It's built
  from PCSX2 (https://pcsx2.net); licenses of the libraries it includes are in licenses\.
  Source code: https://github.com/Bro-Code-Technologies/pcsx2-rombie

Microsoft files
  D3D12\D3D12Core.dll (the DirectX 12 Agility SDK runtime) and msvcp140.dll, vcruntime140.dll and
  vcruntime140_1.dll (the Visual C++ runtime) are Microsoft's, distributed with the engine under
  Microsoft's license terms: licenses\DirectX-Agility-SDK.txt, and the Visual Studio 2022 license
  terms at https://visualstudio.microsoft.com/license-terms/vs2022-ga-community/. You may use them
  only as part of the engine, and you may not modify, reverse engineer or distribute them
  separately. Microsoft provides them as is, without any warranty, and is not liable for any
  damages arising from them.
"@
Set-Content -Path "$stage\README.txt" -Value $readme -Encoding utf8

Compress-Archive -Path $stage -DestinationPath $zip
$size = (Get-Item $zip).Length
"{0} ({1:N1} MB)" -f $zip, ($size / 1MB)
