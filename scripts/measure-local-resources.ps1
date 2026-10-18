param([int]$Seconds=30)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
$start=[System.Diagnostics.Stopwatch]::StartNew()
$samples=@{}
foreach($name in @('GSN','GSW')){
    $path=Join-Path $root "artifacts/$name/$name.exe"
    $p=Get-Process -Name $name -ErrorAction Stop|Where-Object Path -EQ $path|Select-Object -First 1
    if(!$p){throw "Missing local $name process"}
    $samples[$name]=@{pid=$p.Id;cpu=$p.TotalProcessorTime.TotalSeconds;working_set_start=$p.WorkingSet64}
}
Start-Sleep -Seconds $Seconds
$results=@()
foreach($name in @('GSN','GSW')){
    $initial=$samples[$name];$p=Get-Process -Id $initial.pid
    $results += @{process=$name;windows_working_set_mib=[math]::Round($p.WorkingSet64/1MB,2);cpu_percent_single_core=[math]::Round(($p.TotalProcessorTime.TotalSeconds-$initial.cpu)/$start.Elapsed.TotalSeconds*100,3);seconds=$start.Elapsed.TotalSeconds}
}
$report=@{scope='Windows short sample; not Linux/NAS RSS or long-duration acceptance';processes=$results}
$report|ConvertTo-Json -Depth 5|Set-Content -Encoding utf8 (Join-Path $root 'work/local-resource-report.json')
$report|ConvertTo-Json -Depth 5
