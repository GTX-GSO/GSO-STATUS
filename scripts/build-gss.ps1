param([switch]$Flash,[switch]$Benchmark,[switch]$Baseline)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
$benchHeader=Join-Path $root 'main/gss_bench_config.h'
$benchText=if($Benchmark){"#define GSO_GSS_BENCHMARK 1`n"}else{"/* Normal firmware: no benchmark */`n"}
if($Baseline){if(!$Benchmark){throw 'Baseline requires Benchmark'};$benchText+="#define GSO_GSS_BASELINE 1`n"}
if(!(Test-Path -LiteralPath $benchHeader) -or (Get-Content -LiteralPath $benchHeader -Raw) -ne $benchText){[IO.File]::WriteAllText($benchHeader,$benchText)}
Push-Location $root
try {
    . (Join-Path $PSScriptRoot 'esp-idf-env.ps1')
    & $GsoIdfPython $GsoIdfScript build
    if($LASTEXITCODE -ne 0){throw 'GSS build failed'}
    if($Flash){
        & $GsoIdfPython $GsoIdfScript -p COM4 flash
        if($LASTEXITCODE -ne 0){throw 'GSS flash failed'}
    }
} finally {Pop-Location}
