param([int]$Seconds=90,[int]$IntervalMilliseconds=500,[string]$AgentPath='', [string]$OutputName='gsw-resource-latest.json')
$ErrorActionPreference='Stop'
if($Seconds -lt 10 -or $Seconds -gt 86400 -or $IntervalMilliseconds -lt 200){throw 'Invalid measurement window'}
$projectRoot=Split-Path $PSScriptRoot
$agentExecutable=if($AgentPath){(Resolve-Path -LiteralPath $AgentPath).Path}else{Join-Path $projectRoot 'artifacts/GSW/GSW.exe'}
if($OutputName -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*\.json$'){throw 'OutputName must be a JSON file name'}
$sensorPath=Join-Path $env:ProgramFiles 'GSO STATUS/CpuSensor/GSW.exe'
$agent=Get-Process GSW | Where-Object Path -EQ $agentExecutable | Select-Object -First 1
if(!$agent){throw 'Project GSW is not running'}
$clock=[Diagnostics.Stopwatch]::StartNew()
$peaks=@{};$cpu=@{};$seenChildren=@{};$samples=0;$maximumCombined=0L
while($clock.Elapsed.TotalSeconds -lt $Seconds){
    $agent.Refresh();if($agent.HasExited){throw 'GSW exited during measurement'}
    $children=@(Get-CimInstance Win32_Process -Filter "ParentProcessId=$($agent.Id)" | Select-Object -ExpandProperty ProcessId)
    $processes=@($agent)
    $processes+=@(Get-Process GSW -ErrorAction SilentlyContinue | Where-Object Path -EQ $sensorPath)
    foreach($childId in $children){$p=Get-Process -Id $childId -ErrorAction SilentlyContinue;if($p){$processes+=$p;$seenChildren[$childId]=$p.ProcessName}}
    $combined=0L
    foreach($p in $processes){
        try{
            $p.Refresh();$key="$($p.Id):$($p.StartTime.Ticks)"
            $combined+=$p.WorkingSet64
            if(!$peaks.ContainsKey($key)){$peaks[$key]=@{process=$p.ProcessName;role=if($p.Id -eq $agent.Id){'agent'}elseif($p.Path -eq $sensorPath){'sensor'}else{'child'};peak_mib=0}}
            $peaks[$key].peak_mib=[math]::Max($peaks[$key].peak_mib,$p.WorkingSet64/1MB)
            if(!$cpu.ContainsKey($key)){$cpu[$key]=@{first=$p.TotalProcessorTime.TotalSeconds;last=$p.TotalProcessorTime.TotalSeconds}}
            $cpu[$key].last=$p.TotalProcessorTime.TotalSeconds
        }catch [InvalidOperationException]{}
    }
    $maximumCombined=[math]::Max($maximumCombined,$combined);$samples++
    Start-Sleep -Milliseconds $IntervalMilliseconds
}
$elapsed=$clock.Elapsed.TotalSeconds
$used=($cpu.Values | ForEach-Object {$_.last-$_.first} | Measure-Object -Sum).Sum
$report=@{
    scope='Windows GSW, protected CPU worker and observed direct children; working set, not private bytes or Linux RSS'
    seconds=[math]::Round($elapsed,2);samples=$samples;interval_ms=$IntervalMilliseconds
    peak_combined_working_set_mib=[math]::Round($maximumCombined/1MB,2)
    sampled_peak_within_200_mib=($maximumCombined -le 200MB)
    cpu_percent_single_core=[math]::Round($used/$elapsed*100,3)
    cpu_percent_machine=[math]::Round($used/$elapsed*100/[Environment]::ProcessorCount,3)
    observed_child_process_count=$seenChildren.Count
    processes=@($peaks.Values | ForEach-Object {@{process=$_.process;role=$_.role;peak_working_set_mib=[math]::Round($_.peak_mib,2)}})
    limitations='Sampled peaks can miss shorter processes; exited child CPU is a lower bound; this is not two-hour acceptance or fallback-RPC peak acceptance.'
}
$directory=Join-Path $projectRoot 'artifacts/verification'
New-Item -ItemType Directory -Force $directory | Out-Null
$report | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $directory $OutputName)
$report | ConvertTo-Json -Depth 5
