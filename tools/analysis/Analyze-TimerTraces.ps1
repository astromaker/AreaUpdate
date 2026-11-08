param(
    [string]$RootPath = 'artifacts/wheel-resolution',
    [switch]$SelfTest
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
function Convert-TimerTrace($event) {
    $deadline = [int64]$event.deadline_ms
    $submitted = [math]::Max($deadline,[int64]$event.submitted_ms)
    $inserted = [math]::Max($submitted,[int64]$event.inserted_ms)
    $eligible = [math]::Max($inserted,[int64]$event.eligible_ms)
    $poll = [math]::Max($eligible,[int64]$event.poll_ms)
    $selected = [math]::Max($poll,[int64]$event.selected_ms)
    $execute = [int64]$event.execute_ms
    $parts = [ordered]@{
        producerLateMs = $submitted-$deadline
        requestQueueLateMs = $inserted-$submitted
        wheelEligibilityMs = $eligible-$inserted
        pollLateMs = $poll-$eligible
        selectionMs = $selected-$poll
        batchWaitMs = $execute-$selected
    }
    $sum = ($parts.Values | Measure-Object -Sum).Sum
    if ($execute -lt $selected -or $sum -ne [int64]$event.delay_ms) { throw 'Trace timeline does not reconcile' }
    $result = [ordered]@{worker=[int64]$event.worker;actor=[int64]$event.actor;delayMs=[int64]$event.delay_ms}
    foreach ($key in $parts.Keys) { $result[$key]=$parts[$key] }
    $result['dominantStage'] = ($parts.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1).Key
    $previousPoll = [int64]$event.prev_poll_ms
    $result['pollGapMs'] = [math]::Max([int64]0, [int64]$event.poll_ms-$previousPoll)
    $result['previousTimerPhaseMs'] = [math]::Max([int64]0, [int64]$event.prev_timer_end_ms-$previousPoll)
    $result['requestPhaseMs'] = [math]::Max([int64]0, [int64]$event.request_end_ms-[int64]$event.request_begin_ms)
    $result['tickerPhaseMs'] = [math]::Max([int64]0, [int64]$event.ticker_end_ms-[int64]$event.ticker_begin_ms)
    $sleepEnd = [math]::Min([int64]$event.sleep_end_ms,[int64]$event.poll_ms)
    $sleepBegin = [math]::Max([int64]$event.sleep_begin_ms,$previousPoll)
    $result['sleepInPollGapMs'] = [math]::Max([int64]0,$sleepEnd-$sleepBegin)
    $lateSleepEnd = [math]::Min([int64]$event.sleep_end_ms,$execute)
    $lateSleepBegin = [math]::Max([int64]$event.sleep_begin_ms,$deadline)
    $result['lastSleepLateOverlapMs'] = [math]::Max([int64]0,$lateSleepEnd-$lateSleepBegin)
    $queueSleepEnd = [math]::Min([int64]$event.sleep_end_ms,[int64]$event.inserted_ms)
    $queueSleepBegin = [math]::Max([int64]$event.sleep_begin_ms,$submitted)
    $result['queueSleepLateOverlapMs'] = [math]::Max([int64]0,$queueSleepEnd-$queueSleepBegin)
    $result['previousCallbackMs'] = [int64]$event.prev_callback_ms
    $result['longestEarlierCallbackMs'] = [int64]$event.longest_callback_ms
    $result['batchIndex'] = [int64]$event.batch_index
    $result['batchCount'] = [int64]$event.batch_count
    $result['requestDepth'] = [int64]$event.request_depth
    $result['pendingTimers'] = [int64]$event.pending_timers
    return [pscustomobject]$result
}

if ($SelfTest) {
    $event=[pscustomobject]@{worker=0;actor=1;deadline_ms=100;submitted_ms=90;inserted_ms=95;eligible_ms=105;poll_ms=110;selected_ms=112;execute_ms=128;delay_ms=28;prev_poll_ms=90;prev_timer_end_ms=91;request_begin_ms=91;request_end_ms=92;ticker_begin_ms=0;ticker_end_ms=0;sleep_begin_ms=92;sleep_end_ms=110;prev_callback_ms=1;longest_callback_ms=2;batch_index=3;batch_count=4;request_depth=0;pending_timers=4}
    $row=Convert-TimerTrace $event
    if ($row.wheelEligibilityMs-ne5 -or $row.pollLateMs-ne5 -or $row.selectionMs-ne2 -or $row.batchWaitMs-ne16) {throw 'Decomposition test failed'}
    if ($row.lastSleepLateOverlapMs-ne10 -or $row.queueSleepLateOverlapMs-ne0) {throw 'Sleep overlap test failed'}
    $event.submitted_ms=110
    $event.inserted_ms=130
    $event.eligible_ms=130
    $event.poll_ms=135
    $event.selected_ms=135
    $event.execute_ms=140
    $event.delay_ms=40
    $row=Convert-TimerTrace $event
    if ($row.producerLateMs-ne10 -or $row.requestQueueLateMs-ne20 -or $row.batchWaitMs-ne5) {throw 'Late submission test failed'}
    Write-Output 'Timer trace decomposition tests passed'
    return
}

$root=Join-Path $repositoryRoot $RootPath
$rows=@()
foreach ($file in Get-ChildItem $root -Recurse -Filter timer-traces.csv) {
    $relative=[IO.Path]::GetRelativePath($root,$file.FullName).Replace('\','/')
    foreach ($event in Import-Csv $file.FullName) {
        $row=Convert-TimerTrace $event
        $row|Add-Member -NotePropertyName source -NotePropertyValue $relative
        $rows+=$row
    }
}
if ($rows.Count-eq0) { Write-Output 'No timer events above threshold were retained'; return }
$rows|Sort-Object delayMs -Descending|Export-Csv (Join-Path $root 'trace-analysis.csv') -NoTypeInformation
$rows|Sort-Object delayMs -Descending|Select-Object -First 12|Format-Table source,worker,actor,delayMs,dominantStage,producerLateMs,requestQueueLateMs,wheelEligibilityMs,pollLateMs,selectionMs,batchWaitMs,pollGapMs -AutoSize
