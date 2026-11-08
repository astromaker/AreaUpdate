param([string]$Directory='artifacts/radius-latency/trace')
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference='Stop'
$root=Join-Path $repositoryRoot $Directory
function Read-Fields([string]$Line) {
    $value=@{}
    foreach($match in [regex]::Matches($Line,'(\w+)=([^ ]+)')){$value[$match.Groups[1].Value]=$match.Groups[2].Value}
    return $value
}
$samples=@()
$timers=@()
foreach($file in (Get-ChildItem $root -Filter '*.log')) {
    $lines=@(Get-Content $file.FullName)
    $areaRows=@($lines|Where-Object{$_ -match '^LATE_AREA '}|ForEach-Object{Read-Fields $_})
    foreach($line in ($lines|Where-Object{$_ -match '^LATE_ACTOR '})) {
        $actor=Read-Fields $line
        $start=[long]$actor.start_ns
        $end=[long]$actor.end_ns
        $events=@($areaRows|Where-Object{$_.sample -eq $actor.sample})
        $timerMax=0.0
        $queueMax=0.0
        $membersMax=0.0
        $timeline=@()
        foreach($event in $events) {
            $kind=@('wake','queue','begin','members','end')[[int]$event.stage]
            $time=[long]$event.time_ns
            $timerLate=$null
            $queueWait=$null
            if($kind -eq 'begin' -and $time -ge $start -and $time -le $end) {
                if($event.timer -eq '1') {
                    $timerLate=[long]$event.queued_ns/1e6-[long]$event.deadline_ms
                    $timerMax=[math]::Max($timerMax,$timerLate)
                }
                $queueWait=($time-[long]$event.queued_ns)/1e6
                $queueMax=[math]::Max($queueMax,$queueWait)
            }
            if($kind -eq 'members' -and $time -ge $start -and $time -le $end) {
                $begin=$events|Where-Object{$_.x -eq $event.x -and $_.y -eq $event.y -and $_.stage -eq '2' -and [long]$_.time_ns -le $time}|Sort-Object {[long]$_.time_ns} -Descending|Select-Object -First 1
                if($begin){$membersMax=[math]::Max($membersMax,($time-[long]$begin.time_ns)/1e6)}
            }
            $timeline += [pscustomobject]@{relativeMs=($time-$start)/1e6;kind=$kind;area="$($event.x),$($event.y)";origin="$($event.origin_x),$($event.origin_y)";timerLateMs=$timerLate;queueWaitMs=$queueWait;worker=$event.worker;queueWorker=$event.queue_worker;deadlineMs=$event.deadline_ms}
        }
        foreach($moveLine in ($lines|Where-Object{$_ -match "^LATE_MOVE sample=$($actor.sample) "})) {
            $move=Read-Fields $moveLine
            $timeline += [pscustomobject]@{relativeMs=([long]$move.accepted_ns-$start)/1e6;kind='actor_move';area="$($move.from_x),$($move.from_y) -> $($move.to_x),$($move.to_y)";origin='';timerLateMs=$null;queueWaitMs=$null;worker='';queueWorker='';deadlineMs=''}
        }
        $timeline|Sort-Object relativeMs|Export-Csv "$root/$($file.BaseName)-sample$($actor.sample)-timeline.csv" -NoTypeInformation
        $samples += [pscustomobject]@{caseName=$file.BaseName;sample=$actor.sample;actor=$actor.actor;gapMs=[double]$actor.gap_ms;moves=[int]$actor.moves;maxTimerLateMs=$timerMax;maxAreaQueueWaitMs=$queueMax;maxMembershipMs=$membersMax;observerWaitMs=[double]$actor.observer_wait_ms}
    }
    foreach($line in ($lines|Where-Object{$_ -match '^TIMER_TRACE '})) {
        $event=Read-Fields $line
        $covers=[long]$event.sleep_begin_ms -le [long]$event.eligible_ms -and [long]$event.sleep_end_ms -ge [long]$event.eligible_ms
        $timers += [pscustomobject]@{caseName=$file.BaseName;worker=$event.worker;deadlineMs=[long]$event.deadline_ms;eligibleMs=[long]$event.eligible_ms;executeMs=[long]$event.execute_ms;delayMs=[long]$event.delay_ms;roundingMs=[long]$event.eligible_ms-[long]$event.deadline_ms;registrationLateMs=[math]::Max(0,[long]$event.inserted_ms-[long]$event.deadline_ms);sleepCoversEligible=$covers;wakeAfterEligibleMs=$(if($covers){[long]$event.sleep_end_ms-[long]$event.eligible_ms}else{$null});pollAfterSleepMs=[long]$event.poll_ms-[long]$event.sleep_end_ms;executeAfterPollMs=[long]$event.execute_ms-[long]$event.poll_ms;tickerMs=[long]$event.ticker_end_ms-[long]$event.ticker_begin_ms;requestMs=[long]$event.request_end_ms-[long]$event.request_begin_ms;previousCallbackMs=[long]$event.prev_callback_ms;longestCallbackMs=[long]$event.longest_callback_ms}
    }
}
$samples|Export-Csv "$root/late-samples.csv" -NoTypeInformation
$timers|Export-Csv "$root/timer-paths.csv" -NoTypeInformation
$samples|Group-Object caseName|ForEach-Object{[pscustomobject]@{caseName=$_.Name;retained=$_.Count;maxGap=($_.Group|Measure-Object gapMs -Maximum).Maximum;maxTimer=($_.Group|Measure-Object maxTimerLateMs -Maximum).Maximum;maxQueue=($_.Group|Measure-Object maxAreaQueueWaitMs -Maximum).Maximum;maxMembership=($_.Group|Measure-Object maxMembershipMs -Maximum).Maximum}}|Format-Table -AutoSize
