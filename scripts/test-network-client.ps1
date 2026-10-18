$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot
Push-Location $projectRoot
try {
    & ./tools/zig/zig.exe cc -std=c11 -Wall -Wextra -Werror -DCJSON_NESTING_LIMIT=32 -Isimulator -Ishared -Ivendor/cjson tests/network_client_test.c simulator/data_source.c simulator/home_client.c simulator/clash_client.c shared/gso_link.c shared/gso_snapshot.c shared/gso_model.c vendor/cjson/cJSON.c -lwinhttp -lbcrypt -liphlpapi -o work/network-client-test.exe
    if($LASTEXITCODE -ne 0){throw 'Native client test compile failed'}
    python scripts/test-network-client.py
    if($LASTEXITCODE -ne 0){throw 'Native client network behavior failed'}
}finally{Pop-Location}
