param([string]$OutputDirectory = 'artifacts/wait-return/measurements')
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot $OutputDirectory
if (Test-Path $root) { throw '기존 측정 결과를 보존하도록 새로운 출력 경로를 지정하세요.' }
New-Item -ItemType Directory $root | Out-Null
$rows = @()
$keys = @('AREAUPDATE_WAIT_RETURN_DIAGNOSTICS','AREAUPDATE_WORKER_DIAGNOSTICS','AREAUPDATE_ACTIVE_WAIT_US')
$saved = @{}
try {
    foreach ($key in $keys) { $saved[$key] = [Environment]::GetEnvironmentVariable($key,'Process') }
    $env:AREAUPDATE_WORKER_DIAGNOSTICS = '1'
    $env:AREAUPDATE_ACTIVE_WAIT_US = '0'
    $hashes = @()
    foreach ($version in @('baseline','direct')) {
        $hashes += Get-FileHash "$repositoryRoot/artifacts/wait-return/$version/AreaUpdateExample.exe"
    }
    $hashes | Export-Csv "$root/binaries.csv" -NoTypeInformation
    foreach ($seed in @(42,43)) {
        $order = @('baseline-off','baseline-on','direct-off','direct-on')
        if ($seed -eq 43) { [array]::Reverse($order) }
        foreach ($entry in $order) {
            $version,$state = $entry.Split('-')
            $enabled = $state -eq 'on'
            $env:AREAUPDATE_WAIT_RETURN_DIAGNOSTICS = if ($enabled) { '1' } else { '0' }
            $name = "$seed-$entry"
            & "$repositoryRoot/tools/benchmark/Measure-RuntimeCpu.ps1" -Seconds 20 -Actors 70000 -MapSide 320 -MovesPerSecond 2000 `
                -Seed $seed -JoinDurationMs 1000 -JoinIntervalMs 1 -Mode job -WaitMode signal -ObjectSampleStride 256 `
                -DispatcherProfileStride 0 -TimerTraceThresholdMs 0 -MaxPrivateMemoryMB 1500 -AllowStarvation `
                -ExecutablePath "artifacts/wait-return/$version/AreaUpdateExample.exe" -OutputDirectory "$OutputDirectory/$name" | Out-Null
            $row = Get-Content "$root/$name/summary.json" -Raw | ConvertFrom-Json
            if ($row.processExitCode -ne 0 -or $row.neverUpdated -ne 0 -or $row.tasksLive -ne 0 -or
                $row.waitReturnDiagnosticsEnabled -ne $enabled -or $row.objectSampleStride -ne 256) {
                throw "실행/계측 설정 검증 실패: $name"
            }
            if ($enabled) {
                $buckets = @(Import-Csv "$root/$name/wait-returns.csv" | Where-Object scope -eq total)
                $workers = @(Import-Csv "$root/$name/wait-returns.csv" | Where-Object scope -eq worker)
                if ($buckets.Count -notin @(4,5) -or $workers.Count -ne 8*$buckets.Count) { throw '복귀 bucket 누락' }
                if (($buckets | Measure-Object returns -Sum).Sum -ne $row.sleepCount -or
                    ($buckets | Measure-Object late_timer_returns -Sum).Sum -ne $row.sleepPastTimerCount -or
                    ($buckets | Measure-Object late_timer_ms -Sum).Sum -ne $row.sleepPastTimerMs -or
                    ($buckets | Measure-Object notification_samples -Sum).Sum -ne $row.notifiedWaits) {
                    throw "기존 대기 계측과 합계 불일치: $name"
                }
                foreach ($bucket in $buckets) {
                    $same = @($workers | Where-Object kind -eq $bucket.kind)
                    foreach ($field in @('returns','late_timer_returns','late_timer_ms','overshoot_ns')) {
                        if (($same | Measure-Object $field -Sum).Sum -ne [double]$bucket.$field) {
                            throw "Worker별 합계 불일치: $name $field"
                        }
                    }
                }
            }
            $row | Add-Member version $version
            $row | Add-Member waitProbe $state
            $rows += $row
            $rows | Export-Csv "$root/results.csv" -NoTypeInformation
            "$name FPS=$($row.steadyActorFps) CPU=$($row.steadyCpuPercent) lateWaits=$($row.sleepPastTimerCount) timerP99=$($row.objectTimerP99Ms)"
        }
    }
} finally {
    foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key,$saved[$key],'Process') }
}
