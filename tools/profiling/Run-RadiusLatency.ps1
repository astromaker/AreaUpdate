param(
    [ValidateSet('controls','trace','capacity')][string]$Stage = 'controls',
    [string]$OutputDirectory = 'artifacts/radius-latency'
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot "$OutputDirectory/$Stage"
if (Test-Path $root) { throw 'Choose a new output directory' }
New-Item -ItemType Directory "$root/bin" -Force | Out-Null
foreach ($name in @('UpdateComparison','AreaUpdateExample')) {
    Copy-Item "$repositoryRoot/Source/x64/Release/$name.exe" "$root/bin/$name.exe"
    Copy-Item "$repositoryRoot/Source/x64/Release/$name.pdb" "$root/bin/$name.pdb"
}
$saved = @{}
foreach ($key in @('AREAUPDATE_RADIUS_PROFILE','AREAUPDATE_RADIUS_BATCH','AREAUPDATE_RADIUS_TIMER_BATCH','AREAUPDATE_RADIUS_PUBLISH_BATCH','AREAUPDATE_RADIUS_FAST_BUCKET','AREAUPDATE_RADIUS_MERGE_NOTIFY','AREAUPDATE_RADIUS_LATENCY_STRIDE')) { $saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process') }
$rows = @()
try {
    $env:AREAUPDATE_RADIUS_PROFILE='0'
    $env:AREAUPDATE_RADIUS_BATCH='64'
    $env:AREAUPDATE_RADIUS_TIMER_BATCH='1'
    $env:AREAUPDATE_RADIUS_PUBLISH_BATCH='1'
    $env:AREAUPDATE_RADIUS_FAST_BUCKET='1'
    $env:AREAUPDATE_RADIUS_MERGE_NOTIFY='1'
    if ($Stage -eq 'capacity') {
        $env:AREAUPDATE_RADIUS_LATENCY_STRIDE='512'
        foreach ($seed in @(42,43)) {
            $movesList = if ($seed -eq 42) { @(0,8000) } else { @(8000,0) }
            foreach ($moves in $movesList) {
                $name="280000-$seed-moves$moves"
                & "$repositoryRoot/tools/benchmark/Measure-RuntimeCpu.ps1" -Seconds 20 -Actors 280000 -MapSide 640 -MovesPerSecond $moves `
                    -Seed $seed -JoinDurationMs 1000 -JoinIntervalMs 1 -Mode legacy -TimerTraceThresholdMs 20 -AllowStarvation `
                    -ExecutablePath "$OutputDirectory/$Stage/bin/AreaUpdateExample.exe" -OutputDirectory "$OutputDirectory/$Stage/$name" | Out-Null
                $row=Get-Content "$root/$name/summary.json" -Raw | ConvertFrom-Json
                if ($row.taskStatisticsEnabled -ne $true -or $row.neverUpdated -ne 0) { throw 'Invalid capacity trace' }
                $rows += $row
                $rows | Export-Csv "$root/results.csv" -NoTypeInformation
                "$name FPS=$($row.steadyActorFps) maxGap=$($row.maxGapMs) timerMax=$($row.maxTimerDelayMs) requestMax=$($row.maxQueueDelayMs)"
                Get-Content "$root/$name/runtime.log" | Where-Object {$_ -match '^RADIUS_LATENCY '}
            }
        }
    } else {
        $cases=@()
        if ($Stage -eq 'controls') {
            foreach ($seed in @(42,43)) {
                $policies=if($seed -eq 42){@('stationary','moving','settled')}else{@('settled','moving','stationary')}
                foreach ($policy in $policies) { $cases+=@{name="legacy-uniform-$seed-$policy";mode='legacy';layout='uniform';seed=$seed;policy=$policy;seconds=20;trace=0} }
            }
            foreach ($policy in @('stationary','moving','settled')) { $cases+=@{name="legacy-boundary-42-$policy";mode='legacy';layout='boundary';seed=42;policy=$policy;seconds=20;trace=0} }
            foreach ($policy in @('stationary','moving')) { $cases+=@{name="area-uniform-42-$policy";mode='area';layout='uniform';seed=42;policy=$policy;seconds=20;trace=0} }
        } else {
            $cases=@(
                @{name='uniform-moving';mode='legacy';layout='uniform';seed=42;policy='moving';seconds=60;trace=100},
                @{name='boundary-moving';mode='legacy';layout='boundary';seed=42;policy='moving';seconds=60;trace=100},
                @{name='uniform-stationary';mode='legacy';layout='uniform';seed=42;policy='stationary';seconds=30;trace=60},
                @{name='uniform-settled';mode='legacy';layout='uniform';seed=42;policy='settled';seconds=30;trace=100},
                @{name='uniform-stationary-core';mode='legacy';layout='uniform';seed=42;policy='stationary';seconds=30;trace=0;core=10},
                @{name='uniform-moving-core';mode='legacy';layout='uniform';seed=42;policy='moving';seconds=30;trace=0;core=10}
            )
        }
        foreach ($case in $cases) {
            $moves=if($case.policy -eq 'stationary'){0}else{64}
            $settled=if($case.policy -eq 'settled'){1}else{0}
            $core=if($case.ContainsKey('core')){$case.core}elseif($case.trace -ne 0){10}else{0}
            $log="$root/$($case.name).log"
            $process=Start-Process -FilePath "$root/bin/UpdateComparison.exe" -ArgumentList @('--motion-probe',$case.mode,$case.seconds,5000,$case.seed,'synthetic',$case.layout,'none',$moves,$settled,$case.trace,$core) -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.stderr"
            if(-not $process.WaitForExit(($case.seconds+60)*1000)){$process.Kill();$process.WaitForExit();throw 'Latency probe watchdog'}
            if($process.ExitCode -ne 0){throw "Latency probe failed: $($case.name)"}
            $line=Get-Content $log | Where-Object {$_ -match '^MOTION '} | Select-Object -Last 1
            if(-not $line){throw 'Missing motion result'}
            $row=[ordered]@{caseName=$case.name;traceThresholdMs=$case.trace}
            foreach($match in [regex]::Matches($line,'(\w+)=([^ ]+)')){$row[$match.Groups[1].Value]=$match.Groups[2].Value}
            if($row.sent -ne $row.applied -or $row.overlaps -ne '0' -or $row.tasks_live -ne '0' -or [int]$row.move_attempts -ne $moves -or [int]$row.settled_moves -ne $settled){throw 'Latency configuration or invariant mismatch'}
            $rows += [pscustomobject]$row
            $rows | Export-Csv "$root/results.csv" -NoTypeInformation
            Get-Content $log | Where-Object {$_ -match '^TRACE_SUMMARY|^LATE_ACTOR sample=0 '}
            "$($case.name): FPS=$($row.steady_fps) p99=$($row.p99_gap_upper_ms) maxGap=$($row.max_gap_ms) moves=$($row.moves)"
        }
    }
} finally {
    foreach($key in $saved.Keys){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
}
