$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot
Push-Location $projectRoot
try {
    & ./tools/zig/zig.exe cc -std=c11 -Wall -Wextra -Werror -DCJSON_NESTING_LIMIT=32 -Isimulator -Ishared -Ivendor/cjson tests/home_client_test.c simulator/data_source.c simulator/home_client.c simulator/clash_client.c shared/gso_link.c shared/gso_snapshot.c shared/gso_model.c vendor/cjson/cJSON.c -lwinhttp -lbcrypt -liphlpapi -o work/home-client-test.exe
    if($LASTEXITCODE -ne 0){throw 'HOME native client compile failed'}
    python scripts/test-home-client.py
    if($LASTEXITCODE -ne 0){throw 'HOME native client tests failed'}
}finally{Pop-Location}
