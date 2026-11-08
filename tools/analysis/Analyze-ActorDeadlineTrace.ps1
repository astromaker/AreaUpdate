param([string]$Directory = 'artifacts/actor-deadline-followup/queue-profile')
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot $Directory
$rows = @()
foreach ($layout in @('uniform','boundary')) {
    $actors = @()
    $begins = @()
    foreach ($line in (Get-Content "$root/actor-$layout.log")) {
        if ($line -notmatch '^LATE_(ACTOR|AREA) ') { continue }
        $kind = $Matches[1]
        $fields = @{}
        foreach ($match in [regex]::Matches($line,'(\w+)=([^ ]+)')) {
            $fields[$match.Groups[1].Value] = $match.Groups[2].Value
        }
        if ($kind -eq 'ACTOR') { $actors += $fields }
        elseif ($fields.stage -eq '2') { $begins += $fields }
    }
    foreach ($actor in $actors) {
        $begin = $begins | Where-Object {
            $_.sample -eq $actor.sample -and $_.x -eq $actor.to_x -and $_.y -eq $actor.to_y -and
            [int64]$_.time_ns -le [int64]$actor.end_ns
        } | Sort-Object { [int64]$_.time_ns } -Descending | Select-Object -First 1
        if (-not $begin -or [int64]$actor.actor_deadline_ms -eq 0 -or [int64]$begin.queued_ns -eq 0 -or
            [int64]$begin.acquired_ns -eq 0) { continue }
        $actorNs = [int64]$actor.end_ns
        $dueNs = [int64]$actor.actor_deadline_ms * 1000000
        $deadlineNs = [int64]$begin.deadline_ms * 1000000
        $queueNs = [int64]$begin.queued_ns
        $publishedNs = [int64]$begin.published_ns
        $probeNs = [int64]$begin.first_probe_ns
        $acquiredNs = [int64]$begin.acquired_ns
        $beginNs = [int64]$begin.time_ns
        $row = [pscustomobject]@{layout=$layout;sample=$actor.sample;actor=$actor.actor;
            gapMs=[double]$actor.gap_ms;moves=[int]$actor.moves;
            actorLatenessMs=($actorNs-$dueNs)/1000000.0;
            reservationOffsetMs=($deadlineNs-$dueNs)/1000000.0;
            timerMs=($queueNs-$deadlineNs)/1000000.0;
            publishMs=($publishedNs-$queueNs)/1000000.0;
            dispatchMs=($probeNs-$publishedNs)/1000000.0;
            retryMs=($acquiredNs-$probeNs)/1000000.0;
            batchMs=($beginNs-$acquiredNs)/1000000.0;
            callbackMs=($actorNs-$beginNs)/1000000.0;
            radiusFailures=[uint64]$begin.radius_failures;observerWaitMs=[double]$actor.observer_wait_ms}
        $sum = $row.reservationOffsetMs+$row.timerMs+$row.publishMs+$row.dispatchMs+$row.retryMs+$row.batchMs+$row.callbackMs
        if ([Math]::Abs($sum-$row.actorLatenessMs) -gt 0.001) { throw 'Actor lateness decomposition mismatch' }
        $rows += $row
    }
}
if ($rows.Count -eq 0) { throw 'No complete retained Actor trace; use a longer run or lower threshold' }
$rows | Export-Csv "$root/actor-paths.csv" -NoTypeInformation
$rows | Sort-Object gapMs -Descending | Select-Object -First 8 | Format-Table -AutoSize
