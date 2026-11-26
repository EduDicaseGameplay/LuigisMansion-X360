# Compiles the port's own code (Source\runtime, Source\hle, Source\xbox360, ...) and the generated
# tables (Builds\gen\lm_table.cpp) into Builds\obj\rt with the XDK compiler.
#   powershell -ExecutionPolicy Bypass -File Xbox360\build_runtime.ps1 [-Debug]
param([switch]$Debug)
$ErrorActionPreference = 'Stop'
$X = $env:XEDK
if (-not $X) { throw 'XEDK is not set: install the Xbox 360 SDK' }
$env:PATH = "$X\bin\win32;$env:PATH"
$env:INCLUDE = "$X\include\xbox"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$obj = Join-Path $root 'Builds\obj\rt'
New-Item -ItemType Directory -Force $obj | Out-Null
Remove-Item "$obj\*.obj" -ErrorAction SilentlyContinue
$ver = (Get-Content (Join-Path $root 'VERSION.txt') -ErrorAction SilentlyContinue | Select-Object -First 1)
if (-not $ver) { $ver = 'dev' }
$opt = if ($Debug) { '/Od', '/Zi', '/D_DEBUG' } else { '/O2', '/Zi', '/DNDEBUG' }
$files = @(Get-ChildItem "$root\Source" -Recurse -Filter *.cpp | Where-Object { $_.FullName -notmatch 'thirdparty' -and $_.Name -ne 'lm_gx_glue.cpp' -and $_.Name -ne 'lm_gx_platform.cpp' } | ForEach-Object { $_.FullName }) + (Join-Path $root 'Builds\gen\lm_table.cpp')
$trace = if ($env:LM_TRACE -eq '1') { @('/DLM_TRACE') } else { @() }
$flags = @('/nologo', '/c', '/MP', '/TP', '/EHs-c-', '/GF', '/Gy', '/W3', '/wd4996', '/wd4100', '/D_XBOX', "/DLM_VERSION=\`"$ver\`"",
           "/I$root\Source\runtime", "/I$root\Builds\gen", "/Fo$obj\\", "/Fd$obj\vc.pdb") + $opt + $trace
$out = & cl @flags @files 2>&1 | Out-String
if ($env:LM_SHOW_CL -eq "1") { Write-Host $out }
$errs = $out -split "`n" | Where-Object { $_ -match ': (fatal )?error |warning C4' }
$errs | Select-Object -First 40 | ForEach-Object { Write-Host $_.Trim() }
"{0} files, exit {1}" -f $files.Count, $LASTEXITCODE
if ($LASTEXITCODE -ne 0) { exit 1 }
