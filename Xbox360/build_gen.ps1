# Compiles the recompiled game code (Builds\gen\*.cpp, made by Tools\recomp\ppc_recomp.py) into
# Builds\obj\gen\*.obj with the XDK compiler, all files in parallel (/MP).
#   powershell -ExecutionPolicy Bypass -File Xbox360\build_gen.ps1 [-Only lm_code_012] [-Opt /O2]
# The objects hold code translated from your own main.dol: never share them.
param([string]$Only, [string]$Opt = '/O2')
$ErrorActionPreference = 'Stop'
$X = $env:XEDK
if (-not $X) { throw 'XEDK is not set: install the Xbox 360 SDK' }
$env:PATH = "$X\bin\win32;$env:PATH"
$env:INCLUDE = "$X\include\xbox"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$gen = Join-Path $root 'Builds\gen'
$obj = Join-Path $root 'Builds\obj\gen'
New-Item -ItemType Directory -Force $obj | Out-Null
$files = Get-ChildItem $gen -Filter *.cpp | Where-Object { $_.BaseName -ne 'lm_table' -and (-not $Only -or $_.BaseName -eq $Only) } | ForEach-Object { $_.FullName }
if (-not $files) { throw "no generated code in ${gen} - run Tools\recomp\ppc_recomp.py first" }
$trace = if ($env:LM_TRACE -eq '1') { @('/DLM_TRACE') } else { @() }
$flags = @('/nologo', '/c', $Opt, '/MP', '/TP', '/EHs-c-', '/GF', '/Gy', '/W3', '/wd4102', '/wd4101', '/D_XBOX', '/DNDEBUG',
           "/I$root\Source\runtime", "/I$gen", "/Fo$obj\\") + $trace
$t0 = Get-Date
$out = & cl @flags @files 2>&1 | Out-String
$errs = $out -split "`n" | Where-Object { $_ -match ': (fatal )?error ' }
$errs | Select-Object -First 30 | ForEach-Object { Write-Host $_.Trim() }
"{0} files, {1} errors, {2:N0} s" -f $files.Count, $errs.Count, ((Get-Date) - $t0).TotalSeconds
if ($LASTEXITCODE -ne 0) { exit 1 }
