$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot
$recordFile=Join-Path $projectRoot 'work/local-processes.json'
if(!(Test-Path -LiteralPath $recordFile)){Write-Output '没有记录本机联调进程。';exit 0}
$gswConfig=Join-Path $projectRoot 'config/gsw.json'
if(Test-Path -LiteralPath $gswConfig){& (Join-Path $projectRoot 'artifacts/GSW/GSW.exe') --config $gswConfig --stop}
$allowed=@('artifacts/GSN/GSN.exe','artifacts/GSW/GSW.exe','artifacts/GSS-Simulator.exe')|ForEach-Object{Join-Path $projectRoot $_}
foreach($record in @(Get-Content -LiteralPath $recordFile -Raw|ConvertFrom-Json)){
    $process=Get-Process -Id $record.id -ErrorAction SilentlyContinue
    if(!$process -or $process.Path -ne $record.path -or $process.Path -notin $allowed){continue}
    if($process.Path -like '*GSS-Simulator.exe'){[void]$process.CloseMainWindow()}
    try {Wait-Process -Id $process.Id -Timeout 3 -ErrorAction Stop}catch{$still=Get-Process -Id $process.Id -ErrorAction SilentlyContinue;if($still -and $still.Path -eq $record.path){Stop-Process -Id $still.Id}}
}
Remove-Item -LiteralPath $recordFile
Write-Output '本项目记录的本机联调进程已停止。'
