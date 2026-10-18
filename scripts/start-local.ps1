param([switch]$NoDisplay)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot
Push-Location $projectRoot
try {
    foreach($file in @('artifacts/GSN/GSN.exe','artifacts/GSW/GSW.exe','artifacts/GSS-Simulator.exe')) {
        if(!(Test-Path -LiteralPath $file)){throw "缺少程序，请先构建：$file"}
    }
    New-Item -ItemType Directory -Force config,work | Out-Null
    $gsnConfig=Join-Path $projectRoot 'config/gsn.json'
    if(!(Test-Path -LiteralPath $gsnConfig)){
        function New-Token { $buffer=New-Object byte[] 32; $generator=[System.Security.Cryptography.RandomNumberGenerator]::Create();try{$generator.GetBytes($buffer)}finally{$generator.Dispose()};return [BitConverter]::ToString($buffer).Replace('-','') }
        $writeToken=New-Token
        $readToken=New-Token
        @{listen='127.0.0.1:17890';agent_id='desktop';write_token=$writeToken;read_token=$readToken;router_url='http://192.168.124.1/mobile.asp'} | ConvertTo-Json | Set-Content -Encoding utf8 $gsnConfig
    }
    $gsn=Get-Content -LiteralPath $gsnConfig -Raw | ConvertFrom-Json
    if($gsn.listen -ne '127.0.0.1:17890'){throw '本机联调入口要求GSN listen=127.0.0.1:17890；NAS请按独立配置说明操作。'}
    $gswConfig=Join-Path $projectRoot 'config/gsw.json'
    $displayConfig=Join-Path $projectRoot 'config/display.json'
    if(!(Test-Path -LiteralPath $gswConfig)) {@{SnapshotBaseUrl='http://127.0.0.1:17890';AgentId=$gsn.agent_id;WriteToken=$gsn.write_token;GpuIndex=0;Drives=@('C:\','D:\','E:\')}|ConvertTo-Json|Set-Content -Encoding utf8 $gswConfig}
    if(!(Test-Path -LiteralPath $displayConfig)) {@{url='http://127.0.0.1:17890/api/v1/snapshot';read_token=$gsn.read_token}|ConvertTo-Json|Set-Content -Encoding utf8 $displayConfig}
    $processFile=Join-Path $projectRoot 'work/local-processes.json'
    $knownRecords=if(Test-Path -LiteralPath $processFile){@(Get-Content -LiteralPath $processFile -Raw|ConvertFrom-Json)}else{@()}
    function Start-Owned([string]$relative,[string[]]$arguments){
        $exe=Join-Path $projectRoot $relative
        foreach($record in $knownRecords){$existing=Get-Process -Id $record.id -ErrorAction SilentlyContinue;if($existing -and $existing.Path -eq $exe){return $record}}
        if($relative -like '*GSN.exe'){$listener=[System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback,17890);try{$listener.Start()}finally{$listener.Stop()}}
        $style=if($relative -like '*GSS-Simulator.exe'){'Normal'}else{'Hidden'}
        $process=Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $projectRoot -WindowStyle $style -PassThru
        return @{id=$process.Id;path=$exe}
    }
    $records=@(Start-Owned 'artifacts/GSN/GSN.exe' @('--config',('"'+$gsnConfig+'"')))
    for($attempt=0;$attempt -lt 20;$attempt++){try{Invoke-RestMethod 'http://127.0.0.1:17890/healthz' -TimeoutSec 1|Out-Null;break}catch{Start-Sleep -Milliseconds 100}}
    $records+=Start-Owned 'artifacts/GSW/GSW.exe' @('--config',('"'+$gswConfig+'"'))
    if(!$NoDisplay){$records+=Start-Owned 'artifacts/GSS-Simulator.exe' @('--config',('"'+$displayConfig+'"'))}
    else{foreach($record in $knownRecords){if($record.path -eq (Join-Path $projectRoot 'artifacts/GSS-Simulator.exe')){$existing=Get-Process -Id $record.id -ErrorAction SilentlyContinue;if($existing -and $existing.Path -eq $record.path){$records+=$record}}}}
    $records|ConvertTo-Json|Set-Content -Encoding utf8 $processFile
    Write-Output '本机GSN、GSW已启动，数据每秒刷新。配置位于项目config目录；未部署NAS。'
} finally {Pop-Location}
