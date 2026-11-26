# Compiles the GX core and Xenos back end taken from the Super Mario Sunshine 360 port
# (Source\thirdparty\sms_gx, MIT) into Builds\obj\smsgx. Dolphin SDK headers come from the staged
# Sunshine tree (C:\work\sms-xenon\stage) until LuigisMansion-X360 carries its own copy.
#   powershell -ExecutionPolicy Bypass -File Xbox360\build_gx.ps1 [-Only xe_gx_render]
param([string]$Only, [int]$Errors = 30)
$ErrorActionPreference = 'Stop'
$X = $env:XEDK
if (-not $X) { throw 'XEDK is not set' }
$env:PATH = "$X\bin\win32;$env:PATH"
$env:INCLUDE = "$X\include\xbox"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$D = Join-Path $root 'Source\thirdparty\sms_gx'
$S = 'C:\work\sms-xenon\stage'
$obj = Join-Path $root 'Builds\obj\smsgx'
New-Item -ItemType Directory -Force $obj | Out-Null
$units = @("$D\platform\gx\src\gx_fifo.cpp", "$D\platform\gx\src\gx_api.cpp", "$D\platform\gx\src\gx_vert.cpp",
           "$D\xenon\gx\xe_gx_codec.cpp", "$D\xenon\gx\xe_gx_exec.cpp", "$D\xenon\gx\xe_gx_hlsl.cpp",
           "$D\xenon\gx\xe_gx_queue.cpp", "$D\xenon\gx\xe_gx_render.cpp", "$D\xenon\gx\xe_gx_tex.cpp",
           "$root\Source\gx\lm_gx_glue.cpp", "$root\Source\gx\lm_gx_platform.cpp",
           "$root\Source\thirdparty\sms_audio\dsp_mixer.cpp")
if ($Only) { $units = $units | Where-Object { [IO.Path]::GetFileNameWithoutExtension($_) -eq $Only } }
$flags = @('/nologo', '/c', '/O2', '/MP', '/TP', '/EHsc', '/GF', '/Gy', '/W1', '/D_XBOX', '/DGEKKO', '/DLM360', '/DNDEBUG=1', '/DTARGET_PC', '/DTARGET_X360', '/DBUILD_VERSION=2', '/DMASTER', '/DVERSION_GMSE01',
           "/FI$D\xenon\msvc\xe_prelude.h", "/I$D\xenon\msvc\include", "/I$D\platform\gx\port_include",
           "/I$D\platform\gx\include", "/I$D\platform\gx\src", "/I$root\Source\gx", "/I$S\src\port_include", "/I$S\src", "/I$S\platform", "/I$S\game\libs\dolphin\include",
           "/I$S\game\include", "/I$S\src", "/I$S\platform", "/I$S\sdk\include", "/Fo$obj\\")
$out = & cl @flags @units 2>&1 | Out-String
$errs = $out -split "`n" | Where-Object { $_ -match ': (fatal )?error ' }
$errs | Select-Object -First $Errors | ForEach-Object { Write-Host ($_.Trim() -replace '^.*\\(sms_gx|Source)\\', '') }
"{0} units, {1} errors" -f $units.Count, $errs.Count
if ($LASTEXITCODE -ne 0) { exit 1 }
