param([switch]$Test)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot
$sourceDir = Join-Path $projectRoot 'simulator'
$lvglDir = Join-Path $projectRoot 'vendor\lvgl'
$zigExe = Join-Path $projectRoot 'tools\zig\zig.exe'
$workDir = Join-Path $projectRoot 'work'
$outputDir = Join-Path $projectRoot 'artifacts'
foreach($path in @($zigExe,(Join-Path $lvglDir 'lvgl.h'),(Join-Path $sourceDir 'ui.c'))) {
    if(!(Test-Path -LiteralPath $path)) {throw "Missing project dependency: $path"}
}
New-Item -ItemType Directory -Force -Path $workDir,$outputDir | Out-Null
$buildTime = [DateTimeOffset]::UtcNow.ToOffset([TimeSpan]::FromHours(8)).ToString('yyyy-MM-dd HH:mm:ss')
[System.IO.File]::WriteAllText((Join-Path $sourceDir 'build_info.h'),("#pragma once`n#define GSO_VERSION `"0.7.23`"`n#define GSO_BUILD_TIME `""+$buildTime+" UTC+8`"`n"),[System.Text.UTF8Encoding]::new($false))
$arguments = @('-target','x86_64-windows-gnu','-O2','-std=c11','-DLV_CONF_INCLUDE_SIMPLE',('-I'+$sourceDir),('-I'+$lvglDir))
$arguments += @(('-I'+(Join-Path $projectRoot 'shared')),('-I'+(Join-Path $projectRoot 'vendor/cjson')),'-DCJSON_NESTING_LIMIT=32')
if($Test) { $arguments += '-DGSO_TEST' }
$arguments += @(Get-ChildItem -LiteralPath (Join-Path $lvglDir 'src') -Recurse -Filter '*.c' | ForEach-Object FullName)
$arguments += @(Get-ChildItem -LiteralPath $sourceDir -Filter '*.c' | ForEach-Object FullName)
$arguments += @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'shared') -Filter '*.c' | ForEach-Object FullName)
$arguments += (Join-Path $projectRoot 'vendor/cjson/cJSON.c')
$exeName = if($Test) { 'GSS-Test.exe' } else { 'GSS-Simulator.exe' }
$arguments += @('-o',(Join-Path $outputDir $exeName),'-luser32','-lgdi32','-lkernel32','-lwinhttp','-liphlpapi','-lbcrypt','-mwindows','-Wno-unused-command-line-argument')
$responseFile = Join-Path $workDir 'build.rsp'
[System.IO.File]::WriteAllLines($responseFile,($arguments | ForEach-Object { '"'+($_ -replace '\\','/')+'"' }),[System.Text.UTF8Encoding]::new($false))
& $zigExe cc "@$responseFile"
if($LASTEXITCODE -ne 0) {throw "Build failed: $LASTEXITCODE"}
Write-Output "Built artifacts/$exeName"
