# 예전 Area 안정성과 최악 지연 검증

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

## 발견한 문제와 수정

기존 알림 통합은 같은 Worker의 게시 알림을 다음 on_tick까지 미뤘다. 그 사이 긴 timer/request callback이 실행되면, 다른 Worker가 실행할 수 있는 독립 Area도 기다리는 문제가 재현됐다. 콜백이 소비자 작업 완료를 기다리는 구조라면 단순한 평균 성능 문제보다 심각할 수 있다.

수정은 빈 큐에서 비어 있지 않은 큐로 바뀌는 게시를 즉시 알리는 것이다. 이미 작업이 있는 큐의 추가 게시만 통합한다. 여러 bucket을 게시할 때 하나라도 비어 있었으면 전체 게시 후 즉시 알린다. 일반 push의 depth=1도 동일하게 처리한다. 외부 생산자 알림과 실행 완료 알림은 기존대로 유지한다. radius, batch 64, FIFO/선두 중단, Actor 소유권은 변경하지 않았다.

### 200ms 콜백 재현

독립 Area의 게시 시점부터 update 진입까지의 시간이다. 각 5회이며 timer/request는 실행 가능한 다른 Worker가 대기 중인 조건이다. before 바이너리와 로그는 artifacts/radius-stability/before에 보관했다.

| 조건 | 수정 전 merge ON 중앙값 / 최대 | 수정 후 merge ON 중앙값 / 최대 |
|---|---:|---:|
| timer callback, sleep | 201.125 / 212.368ms | 0.037 / 0.053ms |
| request callback, sleep | 206.065 / 215.074ms | 0.019 / 0.028ms |

수정 전 busy callback에서도 중앙값 약 200.03ms가 재현됐다. sleep의 시간 해상도만으로 설명되는 현상이 아니다. merge OFF의 대조군은 같은 조건에서 0.1ms 미만에 실행됐다.

Area callback이 같은 footprint의 잠금을 200ms 보유하는 대조군은 수정 후에도 약 200~215ms 기다렸다. 이는 독립 작업의 알림 지연과 구분해야 한다. 임계 구역을 오래 점유하는 실제 update의 지연을 이번 수정이 제거하지는 않는다. 위 수치는 관측값이며 운영체제 지연까지 포함한 응답 시간 상한을 보장하지 않는다.

## 측정 도구

UpdateComparison --radius-delay-probe에서 timer/request/area × sleep/busy × 10/50/200ms × merge OFF/ON을 각각 5회 비교한다. Task 수명과 종료 시 ready 잔여를 확인한다. 첫 작업 게시 후 생산자 callback을 gate로 막고 소비자가 gate 해제 전에 실행하는지 단위 검사에도 추가했다. 단일 push와 push_batch, timer/request 모두 검사한다.

MotionInteractionProbe에는 정상 측정 구간의 Actor 갱신 간격 히스토그램을 추가했다. 첫 1초와 종료 시 정리·100ms 탈퇴 검사 구간을 제외한다. p99_gap_upper_ms는 10ms bucket의 상계이며 마지막 overflow bucket에서는 실제 최대값을 사용한다. max_gap_ms는 관측 최대다. 지연 원인별 추적이 아니므로 이동 인계/경합/OS 지연의 비중은 이 값만으로 확인 불가다.

## 실행 계획

tools/experiments/Run-RadiusStability.ps1에서 다음을 순차 실행했다. Release x64, Task 통계 ON, Worker 8개, signal, fast 및 타이머/게시 묶음 ON이다. 보완 전 알림 유예가 아니라 수정 후 merge OFF/ON을 비교한다. 새 Area와 공통 Core는 변경하지 않았다.

| 검사 | 사례 | 실행 길이 |
|---|---|---|
| 긴 callback | 36개 조합 × 5회 | callback당 10/50/200ms |
| 고부하 | 28만 Actor, 640×640, seed 42/43, merge OFF/ON | 각 30초 × 4회 |
| 연속 이동·합성 전투 | 5천 Actor, uniform/boundary, merge OFF/ON | 각 300초 × 4회 |
| 이벤트 ID·소유권 검증 | 5천 Actor, uniform/boundary, merge OFF/ON | 각 60초 × 4회 |

연속 synthetic은 이벤트 ID 목록을 저장하지 않으므로 메모리 추이를 관찰할 수 있다. verify는 검증용 expected/applied ID 목록을 계속 보관해 메모리가 증가할 수 있다. 이를 서버 누수와 혼동하지 않는다. 프로세스 private memory 추이만으로 누수 부재를 증명할 수는 없다. 1초 간격 원자료와 첫 30~60초/마지막 30초 평균을 기록한다.

실게임 로직은 제공되지 않았으며, 수분 단위 실험을 장기 운영 전체의 안정성 보장으로 해석하지 않는다. 고부하 결과는 Task 통계 OFF였던 앞선 성능 보고서와 직접 합산하지 않는다.

## 긴 callback 결과

수정 후 36개 조합, 총 180회가 완료됐다. timer/request의 독립 작업 120회는 모두 callback 반환 전에 실행됐고 관측 최대는 0.081ms였다. area의 잠금 대조군 60회는 모두 잠금 보유 callback 반환 이후 실행됐다. 이 차이는 의도된 배타 실행이며 알림으로 해소할 수 있는 지연이 아니다.

## 과부하 검증 결과

Task 통계 ON, 28만 Actor, 각 30초, seed 42/43 평균이다. 이 부하는 20 FPS 처리 용량을 넘어섰다.

| 설정 | FPS | CPU | 두 실행 중 최저 Actor FPS | 관측 최대 간격 | p99 bucket 최대 |
|---|---:|---:|---:|---:|---:|
| 수정 후 merge OFF | 16.155 | 85.64% | 15.43 | 198ms | 80ms |
| 수정 후 merge ON | 16.242 | 80.57% | 14.65 | 227ms | 90ms |

중복 갱신과 미갱신 Actor는 0이었다. CPU는 낮아졌지만 최대 갱신 간격과 최저 Actor FPS가 항상 좋아진 것은 아니다. 첫 게시 지연 수정을 전체 tail latency 해결로 해석하면 안 된다. 이 고부하 관측만으로 최대 지연의 원인별 비중을 확정할 수 없다.

## 5분 연속 이동·전투 결과

5천 Actor, 각 사례를 별도 프로세스에서 300초 연속 실행했다. 네 사례 합계 20분이며 하나의 프로세스를 20분 실행한 것은 아니다. p99는 앞서 설명한 bucket 상계다.

| 배치 | merge | 평균 FPS | CPU | p99 상계 | 관측 최대 | 생성/적용 건수 |
|---|---|---:|---:|---:|---:|---:|
| 균등 | OFF | 19.635 | 6.41% | 90ms | 201ms | 1,085,700 |
| 균등 | ON | 19.635 | 5.54% | 90ms | 202ms | 1,085,524 |
| 경계 집중 | OFF | 19.676 | 1.00% | 100ms | 258ms | 11,758,831 |
| 경계 집중 | ON | 19.674 | 0.96% | 100ms | 213ms | 11,781,515 |

네 실행에서 생성/적용 건수와 피해 합계가 일치했고 overlaps=0, tasks_live=0이었다. 합계 25,711,570건이다. synthetic은 메모리 사용을 제한하기 위해 수신자별 ID 전체를 보관하지 않으며, 정확한 ID/수신자 일치는 별도 verify 사례에서 확인한다.

### 메모리 표본

1초 간격 private memory 표본 평균(MiB)이다.

| 배치 / merge | 30~60초 | 마지막 30초 | 증가량 |
|---|---:|---:|---:|
| 균등 OFF | 11.43 | 11.68 | 0.25 |
| 균등 ON | 11.40 | 11.61 | 0.21 |
| 경계 OFF | 10.61 | 10.67 | 0.06 |
| 경계 ON | 10.61 | 10.66 | 0.05 |

이 범위에서는 작은 증가 후 말미가 대체로 평탄했지만, allocator/컨테이너 고수위와 누수를 분리한 할당 추적은 하지 않았다. 누수 부재를 증명하는 결과로 해석하지 않는다. verify의 ID 목록 메모리는 이 표에 포함하지 않는다.

## 수신자별 ID·소유권 검사

각 60초, seed 42, 5천 Actor다. 모든 사례에서 수신자별 expected/applied ID 전체와 피해 합계가 일치했다. 탈퇴 후 update 중단, 이벤트가 대기 중인 Actor 재등록, 재등록 이후 이벤트 처리를 검사하는 ownership_cycle=1이었다.

| 배치 | merge | FPS | p99 상계 | 관측 최대 | 검증 ID 건수 |
|---|---|---:|---:|---:|---:|
| 균등 | OFF | 19.657 | 90ms | 189ms | 596,744 |
| 균등 | ON | 19.648 | 90ms | 202ms | 602,248 |
| 경계 집중 | OFF | 19.685 | 100ms | 216ms | 680,200 |
| 경계 집중 | ON | 19.686 | 100ms | 250ms | 684,424 |

합계 2,563,616건이며 모두 overlaps=0, tasks_live=0이었다. ID 기록 때문에 증가하는 verify 메모리는 누수 판정에 사용하지 않았다. 60초 사례에서는 첫/마지막 메모리 집계 구간도 겹친다.

## 최종 판단

- 실제 문제 1건을 재현하고 수정했다. 빈 큐의 첫 게시를 긴 callback 뒤까지 미루지 않는다. 알림 통합은 이 보호 조건을 포함해 기본 ON을 유지한다.
- 이 범위에서 교착, Actor 동시 update, 이벤트 수신자/ID 불일치, Task 수명 불일치는 관측하지 못했다.
- 5천 Actor 이동 부하 평균은 약 19.6~19.7 FPS이고 최대 갱신 간격은 258ms였다. 무결성 통과와 20 FPS/50ms 시간 보장은 별개다. 특히 50ms 최대 간격 목표는 이 결과로 충족됐다고 볼 수 없다.
- 남은 갱신 지연에는 이동 인계, 예약·큐·공간 충돌, OS 스케줄링이 포함될 수 있지만, 이번 집계만으로 각 원인의 비중은 확인 불가다. 원인별 추적을 하지 않은 값을 특정 원인으로 단정하지 않는다.
- long은 seed 42의 수분 단위 합성 부하다. 실게임, 다시간 연속 운영, address/thread sanitizer 검증은 수행하지 않았다. 무누수나 무데이터레이스의 증명은 아니다.

## 빌드와 재현

- Source/AreaUpdate.sln MSBuild Release/Debug x64 빌드, AreaTests, UpdateComparison --test, tools/validation/Test-RadiusFollowup.ps1 검증 통과.
- timer/request × push/push_batch의 첫 게시 gate 검사에서, 생산자 callback이 막혀 있어도 독립 소비자가 진행함을 확인했다.
- tools/quality/Format-Code.ps1 -All -Check, tools/quality/Check-CodeStyle.ps1 통과. C++의 기존 BOM 및 CRLF를 보존했다.
- artifacts/radius-stability/before는 수정 전 재현 바이너리/로그, delay는 36조합 원자료, capacity는 28만 검증, motion은 8개 연속 부하 결과와 1초 메모리 표본, validation은 회귀 검사 로그다.

tools/experiments/Run-RadiusStability.ps1의 delay, capacity, motion 단계로 재현할 수 있다. 같은 출력 디렉터리는 덮어쓰지 않는다.
