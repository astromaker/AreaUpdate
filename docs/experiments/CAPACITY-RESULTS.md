# 20 FPS 미달과 CPU 한계 계측

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

이 문서는 Actor 수 70,000개를 고정한 합성 연산량 증가 실험이다.
후속 Actor 수 증가·밀도 유지 확장 테스트는 ACTOR-SCALE-RESULTS.md를 참고한다.

## 결론

CPU 사용률이 낮고 FPS가 부족하다는 사실만으로 잘못된 sleep을 확정할 수는 없다.
실행 가능한 작업이 남은 경우와 공간 실행 제한 때문에 당장 실행할 수 없는 경우를 구분해야 한다.
이번에는 그 구분을 계측했고, 현 Area 경로에서 partition 제한 상태의 대기가 많이 발생했다.
CPU 계산 대조군은 약 93.8%를 사용했지만 Area 경로는 시험 범위에서 약 55%까지만 사용했다.
따라서 Area 경로를 장비 CPU의 처리 한계까지 밀어붙였다고 주장할 수는 없다.
CPU가 포화되기 전에 갱신률·작업 배분·대기 정책의 한계가 나타났다.

## 변경 사항

- Worker에 sleep 직전 실행 가능 상태, partition 제한 상태, 타이머 시각 초과 계측을 추가했다.
- TickerIdleState는 AreaExecuter의 partition mutex 안에서 상태를 읽는다.
- Actor에 50ms 초과 갱신 간격, 목표 업데이트 부족량과 목표 미달 Actor 수를 기록했다.
- 이동 예제의 마지막 선택 인수 actorWork로 Actor별 의존적인 xorshift 연산 횟수를 조절한다.
  기본값은 0이다. 결과를 Actor에 저장하고 최종 checksum에 사용해 계산이 제거되지 않게 했다.
- UpdateComparison --cpu-control은 큐·partition 없이 논리 CPU 수만큼 스레드를 생성해 계산한다.
- scheduling, batch 64, partition admission 상한 8, 공평성, 소유권, round-robin 정책은 변경하지 않았다.

## 측정 조건

2026-10-31, MSBuild Release x64, 논리 CPU 8개, Worker 8개.
Actor 70,000개, 320×320 Area, radius 2, 이동 시도 초당 2,000회, seed 42.
Actor를 첫 5,000ms 동안 1ms 확인 간격으로 분산 입장시켰다.
CPU는 전체 논리 CPU를 합쳐 100%로 환산한 실제 프로세스 CPU 시간이다.
정상 상태 CPU는 시작 6초 이후의 250ms 표본을 시간 가중해 계산했다.
FPS는 입장 후 Actor 시간 합계를 분모로 한 전체 실행 평균이므로 CPU 표본 구간과 정확히 같지는 않다.
각 연산량은 1회 탐색 측정이며 통계적으로 확정적인 성능 수치는 아니다.

## 부하 증가 결과

| Actor당 연산 횟수 | 실행 시간 | 정상 상태 CPU | 평균 Actor FPS | partition 제한 상태의 sleep 비율 | 미갱신 Actor |
|---:|---:|---:|---:|---:|---:|
| 0 | 20초 | 11.46% | 15.164 | 70.26% | 0 |
| 1,000 | 20초 | 15.68% | 7.462 | 96.74% | 0 |
| 5,000 | 20초 | 16.87% | 2.154 | 98.80% | 0 |
| 20,000 | 20초 | 18.14% | 0.632 | 99.61% | 0 |
| 100,000 | 30초 | 47.99% | 0.340 | 99.87% | 0 |
| 500,000 | 30초 | 55.03% | 0.081 | 99.79% | 1,231 |

미갱신은 한 번도 update가 호출되지 않은 Actor 수다.
모든 단계에서 20 FPS 목표 미달 Actor는 70,000개였다. 목표 미달 판정에는 업데이트 1회의 여유를 둔다.
500,000 단계는 정상 통과가 아니다. never_updated=1,231로 실행 파일이 종료 코드 3을 반환했다.
overlaps/tasks_pending/tasks_live는 해당 단계도 0이었다.

마지막 단계는 strict 측정기가 실패 후 summary 생성을 중단해,
저장돼 있던 raw log와 CPU samples에서 summary를 복구했다.
recoveredFromLogs=true이며 전체 CPU 시간·전체 평균 CPU는 null로 남겼다.
표의 정상 상태 CPU는 유효한 시간별 CPU 표본으로 계산한 값이다.
이후 측정기에는 명시적인 -AllowStarvation 옵션을 추가했다.
이 옵션도 중복 실행·잔여 Task는 허용하지 않으며 미갱신 상태와 종료 코드 3을 결과에 남긴다.

## sleep 지표

| 지표 | 의미 |
|---|---|
| ready_samples | 최종 재검사 후, sleep 직전 추가 표본에서 실행 가능한 요청·타이머·Ticker 실행 기회가 관측된 횟수 |
| blocked_samples | 전역 Area 큐는 비어 있지 않지만 partition admission 제한으로 실행 기회가 없는 상태에서 sleep에 들어간 횟수 |
| past_timer_count | sleep 반환 때, 잠들기 전에 알고 있던 다음 로컬 타이머 시각을 넘긴 횟수 |
| past_timer_ms | 위 타이머 시각을 넘긴 지연의 합. Actor별 지연 합계가 아님 |
| overshoot_ms | 요청한 1ms보다 더 걸린 sleep 구간 시간의 합 |
| wake_requests | sleep 반환 때 Worker 요청 큐가 비어 있지 않았던 횟수 |

표본은 실행 전체와 종료 직전 구간을 포함한다. blocked 비율은 시간 비율이 아니라 횟수 비율이다.
ready/blocked 분류와 past_timer_count는 서로 겹칠 수 있으므로 비율을 더하면 안 된다.
샘플 이후 다른 스레드가 상태를 변경할 수 있어, blocked 상태가 sleep 내내 유지됐다고 볼 수 없다.
ready_samples도 직전 재검사와 추가 표본 사이에 새 작업이 생긴 경우를 포함한다.
모든 Actor의 목표 미달과 각 sleep의 동시성을 개별 타임라인으로 연결한 것은 아니다.

부하 0 단계에서는 sleep 13,793회 중 ready 26회, blocked 9,691회,
타이머 시각 초과 5,215회였다. 부하 5,000 단계는 ready가 0회인데도
FPS가 2.154이고 blocked가 98.80%였다.
따라서 이미 실행 가능한 일을 놓치는 경우만으로 전체 FPS 손실을 설명하기 어렵다.

## 코드상 확인된 제약

AreaExecuter::has_work는 ready Area가 있어도 admission 시도 수가 8이면 false를 반환한다.
마지막 runner가 종료하고 partition이 바뀔 때까지 신규 진입을 막는 기존 정책이다.
다른 partition에 작업이 남았다고 임의로 동시에 실행하면 공간 안전성이 깨질 수 있다.

또한 Worker::wake는 현재 sleep을 중단하지 않고 유휴 상태 표시만 바꾼다.
대기 중 제한이 해제돼도 다른 Worker는 sleep 반환 전까지 참여하지 못할 수 있다.
계측만으로 이 각각의 영향 비율을 확정하지는 못했지만 우선적인 A/B 비교 대상이다.

별도로 현재 Area 재예약은 update 완료 + 50ms다.
Actor 연산·큐 대기·timer wheel 5ms 단위 올림·sleep 지연이 더해지므로
일정한 시작 간격 50ms를 보장하는 20Hz 스케줄링과는 다르다.
CPU가 남는다고 단순히 sleep만 없애면 목표를 달성한다고 결론낼 수 없는 이유다.

## CPU 대조군

1. 기존 ObjectUpdate: 70,000 Actor, 10 tick, 상호작용 0, work 100,000, Worker 8개.
   측정 구간 19.167초, CPU 시간 127.484초: 전체 기준 약 83.14%.
   violations=0. 실제 Area 경로와 스케줄링·워크로드가 완전히 동일한 성능 비교는 아니다.
2. 큐 없는 계산 대조군: 8스레드, 약 10초, CPU 93.835%.
   장비에서 Area 경로보다 훨씬 높은 CPU 사용이 가능함을 확인하는 용도다.

100%를 정확히 달성한 것은 아니다. 다른 시스템 부하·스케줄링 조건은 통제하지 않았다.
또한 busy-spin으로 CPU 수치만 높이는 것은 개선이 아니다.
같은 Actor 연산량에서 실제 갱신 처리량과 지연이 개선되는지 비교해야 한다.

## 재현과 다음 검증

```powershell
# 마지막 인수가 Actor 연산 횟수
.\Source\x64\Release\AreaUpdateExample.exe 20 70000 2000 42 5000 1 1000
.\Measure-RuntimeCpu.ps1 -Seconds 20 -ActorWork 1000 -OutputDirectory artifacts/new-capacity-run
.\Source\x64\Release\UpdateComparison.exe --cpu-control 10
```

Run-Capacity.ps1은 부하를 증가시키며 두 차례 정상 상태 CPU 90% 이상이면 탐색을 종료한다.
이번에는 해당 조건에 도달하지 않았다. 기존 artifacts/capacity를 덮어쓰지 않도록 보호한다.
results.csv, 단계별 summary.json/runtime.log/samples.csv와 측정 실행 파일·해시는
artifacts/capacity에 보존했다. 전환 전 소스도 before-source에 보존했다.

다음에는 동일 부하에서 partition 제한 해제 후 Worker 재참여 지연과,
완료 기준 +50ms 대 절대 주기 예약을 각각 분리해 비교해야 한다.
이번 작업은 계측과 한계 탐색이며 실행 정책을 수정한 패치는 아니다.
