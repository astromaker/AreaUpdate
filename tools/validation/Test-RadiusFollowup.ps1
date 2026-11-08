param([string]$OutputDirectory = 'artifacts/radius-followup-v2/validation')
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot $OutputDirectory
if (Test-Path $root) { throw 'Choose a new output directory' }
New-Item -ItemType Directory $root -Force | Out-Null
$saved = @{}
foreach ($key in @('AREAUPDATE_RADIUS_PROFILE','AREAUPDATE_RADIUS_BATCH','AREAUPDATE_RADIUS_TIMER_BATCH','AREAUPDATE_RADIUS_PUBLISH_BATCH','AREAUPDATE_RADIUS_FAST_BUCKET','AREAUPDATE_RADIUS_MERGE_NOTIFY')) {
    $saved[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
}
try {
    $env:AREAUPDATE_RADIUS_PROFILE = '0'
    $env:AREAUPDATE_RADIUS_BATCH = '64'
    $env:AREAUPDATE_RADIUS_TIMER_BATCH = '1'
    $env:AREAUPDATE_RADIUS_PUBLISH_BATCH = '1'
    $env:AREAUPDATE_RADIUS_FAST_BUCKET = '1'
    $env:AREAUPDATE_RADIUS_MERGE_NOTIFY = '1'
    & "$repositoryRoot/Source/x64/Release/AreaUpdateExample.exe" 2 256 200 42 100 1 0 16 legacy 0 signal | Tee-Object "$root/runtime.log" | Out-Null
    if ($LASTEXITCODE -ne 0 -or (Get-Content "$root/runtime.log" -Raw) -notmatch 'task_statistics=1') { throw 'Validation requires Task statistics ON and a successful runtime smoke test' }
    foreach ($configuration in @('Release','Debug')) {
        & "$repositoryRoot/Source/x64/$configuration/AreaTests.exe" *> "$root/$configuration-AreaTests.log"
        if ($LASTEXITCODE -ne 0) { throw "AreaTests failed: $configuration" }
        "AreaTests $configuration passed"
    }
    & "$repositoryRoot/Source/x64/Release/UpdateComparison.exe" --test *> "$root/comparison-tests.log"
    if ($LASTEXITCODE -ne 0) { throw 'Comparison self-tests failed' }
    foreach ($fast in @(0,1)) {
        foreach ($merge in @(0,1)) {
            $env:AREAUPDATE_RADIUS_FAST_BUCKET = "$fast"
            $env:AREAUPDATE_RADIUS_MERGE_NOTIFY = "$merge"
            $name = "tuning-fast$fast-merge$merge"
            & "$repositoryRoot/Source/x64/Release/UpdateComparison.exe" --motion-probe legacy 3 5000 42 verify uniform *> "$root/$name.log"
            if ($LASTEXITCODE -ne 0) { throw "Tuning validation failed: $name" }
            Get-Content "$root/$name.log" | Where-Object { $_ -match '^MOTION ' }
        }
    }
    $env:AREAUPDATE_RADIUS_FAST_BUCKET = '1'
    $env:AREAUPDATE_RADIUS_MERGE_NOTIFY = '1'
    foreach ($publish in @(0,1)) {
        $env:AREAUPDATE_RADIUS_PUBLISH_BATCH = "$publish"
        foreach ($grouped in @(0,1)) {
            $env:AREAUPDATE_RADIUS_TIMER_BATCH = "$grouped"
            foreach ($batch in @(1,4,16,64)) {
                $env:AREAUPDATE_RADIUS_BATCH = "$batch"
                $name = "uniform-publish$publish-group$grouped-batch$batch"
                & "$repositoryRoot/Source/x64/Release/UpdateComparison.exe" --motion-probe legacy 3 5000 42 verify uniform *> "$root/$name.log"
                if ($LASTEXITCODE -ne 0) { throw "Motion validation failed: $name" }
                Get-Content "$root/$name.log" | Where-Object { $_ -match '^MOTION ' }
            }
        }
    }
    $env:AREAUPDATE_RADIUS_BATCH = '64'
    $env:AREAUPDATE_RADIUS_TIMER_BATCH = '1'
    $env:AREAUPDATE_RADIUS_PUBLISH_BATCH = '1'
    foreach ($workload in @('verify','synthetic')) {
        foreach ($mode in @('area','legacy')) {
            foreach ($seed in @(42,43)) {
                $name = "boundary-$workload-$mode-$seed"
                & "$repositoryRoot/Source/x64/Release/UpdateComparison.exe" --motion-probe $mode 5 5000 $seed $workload boundary *> "$root/$name.log"
                if ($LASTEXITCODE -ne 0) { throw "Boundary validation failed: $name" }
                Get-Content "$root/$name.log" | Where-Object { $_ -match '^MOTION ' }
            }
        }
    }
    $process = Start-Process -FilePath "$repositoryRoot/Source/x64/Release/UpdateComparison.exe" -ArgumentList @('--motion-probe','legacy',2,64,42,'verify','boundary','event-id') -WindowStyle Hidden -PassThru -RedirectStandardOutput "$root/fault.log" -RedirectStandardError "$root/fault.stderr"
    if (-not $process.WaitForExit(30000)) { $process.Kill(); $process.WaitForExit(); throw 'Fault validation watchdog' }
    if ($process.ExitCode -eq 0 -or (Get-Content "$root/fault.stderr" -Raw) -notmatch 'Event missing, duplicate or misdirected') { throw 'Injected corruption was not rejected' }
    'Injected event corruption rejected; all radius validations passed'
} finally {
    foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key, $saved[$key], 'Process') }
}
