# Links Builds\xex\default.xex from Builds\obj\rt (build_runtime.ps1) and Builds\obj\gen (build_gen.ps1).
# The XEX holds code translated from your own main.dol: never share it.
#   powershell -ExecutionPolicy Bypass -File Xbox360\build_xex.ps1
$ErrorActionPreference = 'Stop'
$X = $env:XEDK
if (-not $X) { throw 'XEDK is not set: install the Xbox 360 SDK' }
$env:PATH = "$X\bin\win32;$env:PATH"
$env:LIB = "$X\lib\xbox"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$out = Join-Path $root 'Builds\xex'
New-Item -ItemType Directory -Force $out | Out-Null
$objs = @(Get-ChildItem "$root\Builds\obj\rt" -Filter *.obj | ForEach-Object { $_.FullName }) +
        @(Get-ChildItem "$root\Builds\obj\gen" -Filter *.obj | ForEach-Object { $_.FullName }) +
        @(Get-ChildItem "$root\Builds\obj\smsgx" -Filter *.obj -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
$libs = 'xapilib.lib', 'd3d9.lib', 'd3dx9.lib', 'xgraphics.lib', 'xaudio2.lib', 'x3daudio.lib', 'xmcore.lib', 'xinput2.lib', 'xboxkrnl.lib'
# 4 MiB main stack: every guest call is a host call, and the game nests deeply during loads.
& link /nologo /DEBUG /OPT:REF /OPT:ICF /STACK:0x400000 /OUT:"$out\lm360.exe" /PDB:"$out\lm360.pdb" @objs @libs 2>&1 |
    Where-Object { $_ -notmatch 'LNK4075' } | Select-Object -First 40 | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) { throw 'link failed' }
& imagexex /nologo /IN:"$out\lm360.exe" /OUT:"$out\default.xex"
if ($LASTEXITCODE -ne 0) { throw 'imagexex failed' }
Get-Item "$out\default.xex" | Select-Object FullName, Length
