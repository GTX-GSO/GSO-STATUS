param([switch]$ProbeOnly,[int]$TimeoutSeconds=10)
$ErrorActionPreference='Stop'
$rtssExe=Join-Path ${env:ProgramFiles(x86)} 'RivaTuner Statistics Server\RTSS.exe'
$result=[ordered]@{installed=(Test-Path -LiteralPath $rtssExe);started=$false;running=$false;shared_memory=$false;error=$null}
try {
    if(!$result.installed){throw 'RTSS executable was not found'}
    $existing=Get-Process -Name RTSS -ErrorAction SilentlyContinue | Where-Object Path -EQ $rtssExe | Select-Object -First 1
    if(!$existing -and !$ProbeOnly){
        $existing=Start-Process -FilePath $rtssExe -WorkingDirectory (Split-Path $rtssExe) -WindowStyle Hidden -PassThru
        $result.started=$true
    }
    $deadline=[DateTime]::UtcNow.AddSeconds($(if($ProbeOnly){0}else{$TimeoutSeconds}))
    do {
        $result.running=[bool](Get-Process -Name RTSS -ErrorAction SilentlyContinue | Where-Object Path -EQ $rtssExe | Select-Object -First 1)
        try {
            $mapping=[System.IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting('RTSSSharedMemoryV2',[System.IO.MemoryMappedFiles.MemoryMappedFileRights]::Read)
            try {
                $view=$mapping.CreateViewAccessor(0,8,[System.IO.MemoryMappedFiles.MemoryMappedFileAccess]::Read)
                try {$result.signature=$view.ReadUInt32(0);$result.memory_version=$view.ReadUInt32(4);$result.shared_memory=$result.signature -eq 0x52545353} finally {$view.Dispose()}
            } finally {$mapping.Dispose()}
        } catch [System.IO.FileNotFoundException] {}
        if($result.shared_memory -or [DateTime]::UtcNow -ge $deadline){break}
        Start-Sleep -Milliseconds 250
    } while($true)
} catch {$result.error=$_.Exception.Message}
$result | ConvertTo-Json -Compress
if(!$result.running -or !$result.shared_memory){exit 2}
