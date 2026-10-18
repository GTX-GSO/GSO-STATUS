$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot
Push-Location $projectRoot
try {
    New-Item -ItemType Directory -Force work | Out-Null
    & ./tools/zig/zig.exe cc -std=c11 -Wall -Wextra -Werror -Ishared shared/gso_model.c tests/model_test.c -o work/model-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Model compile failed' }
    & ./work/model-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Model tests failed' }
    & python ./scripts/verify-shared-code.py
    if ($LASTEXITCODE -ne 0) { throw 'Simulator and firmware sources diverged' }
    & ./tools/zig/zig.exe cc -std=c11 -Wall -Wextra -Werror -Ishared shared/gso_link.c tests/link_test.c -o work/link-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Link compile failed' }
    & ./work/link-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Link tests failed' }
    & ./tools/zig/zig.exe cc -std=c11 -Wall -Wextra -Werror -Ishared shared/gso_level.c tests/level_test.c -o work/level-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Level compile failed' }
    & ./work/level-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Level tests failed' }
    & ./tools/zig/zig.exe cc -std=c11 -Wall -Wextra -Werror tests/present_test.c -o work/present-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Presentation compile failed' }
    & ./work/present-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Presentation tests failed' }
    & ./tools/zig/zig.exe cc -std=c11 -Wall -Wextra -Werror -Ishared shared/gso_model.c shared/gso_brightness.c tests/brightness_test.c -o work/brightness-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Brightness compile failed' }
    & ./work/brightness-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Brightness tests failed' }
    & ./tools/zig/zig.exe cc -std=c11 -Wall -Wextra -Werror -Ishared -Isimulator -Ivendor/cjson shared/gso_model.c shared/gso_brightness.c simulator/display_brightness.c vendor/cjson/cJSON.c tests/brightness_preferences_test.c -o work/brightness-preferences-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Brightness preferences compile failed' }
    & ./work/brightness-preferences-test.exe (Join-Path $projectRoot 'work/brightness-test-config')
    if ($LASTEXITCODE -ne 0) { throw 'Brightness preferences tests failed' }
    & ./tools/zig/zig.exe cc -std=c11 -Wall -Wextra -Werror -DCJSON_NESTING_LIMIT=32 -Ishared -Ivendor/cjson shared/gso_model.c shared/gso_snapshot.c vendor/cjson/cJSON.c tests/snapshot_test.c -o work/snapshot-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Snapshot parser compile failed' }
    foreach($fixture in @('snapshot-unconfigured.json','snapshot-offline.json','snapshot-vibe-running.json','snapshot-vibe-timeout.json','snapshot-home.json','snapshot-weather-clocks.json','snapshot-game-icon.json','snapshot-net-details.json','snapshot-home-rich.json','snapshot-ambient.json','snapshot-vibe-claude.json','snapshot-printer.json','snapshot-car.json','snapshot-nas.json')) {
        & ./work/snapshot-test.exe "contracts/v1/$fixture"
        if ($LASTEXITCODE -ne 0) { throw 'Snapshot parser tests failed' }
    }
} finally { Pop-Location }
