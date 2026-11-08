# 예전 Area 동일 bucket 및 알림 통합 비교

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

후속 [안정성·최악 지연 검증](RADIUS-STABILITY-RESULTS.md)에서 긴 callback에 따른 첫 게시 지연이 발견돼, 빈 큐의 첫 작업은 즉시 알리도록 보완했다. 아래 수치는 보완 전 성능 실험 기록이다.

## 범위

이번 기준선 base는 앞선 묶음 게시 최적화가 적용된 legacy다. 개별 예약 버전이나 Git 원본을 기준으로 하지 않는다. 새 Area, 공통 Core/Worker, 원본 Baseline, Actor 소유권, radius 2, 추출 batch 64, 큐 선두 충돌 중단 정책은 변경하지 않는다.

## 후보

### 동일 bucket 경로

push_batch의 검증 단계에서 정규화한 bucket이 모두 같은지 확인한다. 같으면 정렬을 생략하고 같은 큐에 입력 순서대로 게시한다. bucket이 섞여 있으면 기존 정렬 경로로 처리한다. 기존 입력 검증, ready 예약, 부분 실패 처리, 공개 후 알림은 유지한다.

### 알림 통합

코드 확인 결과, flush_radius_timers의 Worker::add_task_at 알림은 현재 Worker 자신을 향한다. 이 호출을 실행 완료의 다른 Worker 깨우기와 같은 대상으로 보고 삭제하지 않았다.

대신 같은 RadiusExecuter에 등록된 Worker가 만료 Task 등을 처리하며 발생시키는 게시 알림을 thread-local pendingWake로 표시하고 다음 on_tick 진입에서 한 번에 보낸다. 기존에 게시 알림을 요청하던 지점의 generation 증가는 그대로 유지한다. 해당 Worker가 등록된 것을 첫 on_tick에서 확인하며, 외부 스레드·다른 executor의 Worker·종료 중 Worker의 게시 알림은 즉시 보낸다. 실행 중 게시도 batch 종료 알림에 합치고, on_tick의 조기 실패 경로에 들어가기 전 pending 알림을 처리한다.

Source/Core/Worker.cpp의 Worker::run은 run_timer와 run_task 이후 항상 on_tick을 호출하고 그 뒤 종료 상태를 검사한다. 이를 근거로 같은 Worker의 게시 알림만 유예한다. callback 예외 복구는 기존처럼 지원하지 않는다. 타이머 만료 처리 구간이 길어지면 알림이 지연될 수 있으므로 FPS뿐 아니라 갱신 편차를 비교한다. idle Worker 명단이나 stealing은 추가하지 않는다.

## 검증 방법

- fast/merge 0/1 조합별로 대기 직전 알림, 부분 게시 후 마지막 Runner 대기, 혼합 bucket FIFO, 동종 bucket FIFO, 복수 생산자 검사를 수행한다.
- 자신의 Worker Task 안에서 128개 Area를 두 번에 나눠 게시하고 자신을 stop시킨다. 남은 Worker가 모든 작업을 처리하고 pending 알림이 실제 flush되는지 확인한다.
- 진단 카운터 deferred는 유예된 게시 알림 수, flushes는 on_tick 진입 시 수행한 pending 알림 flush 수다. batch 종료의 기존 완료 알림은 이 flushes에 포함하지 않는다.

## 측정

Run-RadiusFast.ps1은 area(새 Area), base(앞선 묶음 게시), fast(동일 bucket), merged(fast+알림 통합), notify(알림 통합만)를 비교한다. Task 통계와 잠금 계측은 성능 실행에서 끄고, profile 단계만 별도로 계측한다. Worker 8개, signal, Actor 추가 연산 0, 절대 50ms 예약, 1초 동안 1ms 간격 입장이다. seed 42/43에서 실행 순서를 반전한다. 실제 게임 로직 성능은 확인 불가다.

후속 final/boundary의 후보는 사전 비교 후 선택한다. 과거 Run-RadiusPublication.ps1과 Run-RadiusFollowup.ps1은 새 두 옵션을 0으로 고정해 당시 비교 의미를 보존한다. 원자료와 고정 실행 파일/PDB는 artifacts/radius-fast 아래에 보관한다.

## 사전 결과

각 15초, seed 42/43 평균이다.

| 구성 | 28만 FPS | CPU |
|---|---:|---:|
| 새 Area | 18.798 | 76.45% |
| base | 15.660 | 82.52% |
| fast | 16.317 | 85.10% |
| merged | 18.106 | 87.58% |

같은 bucket 경로만으로 일관된 FPS/CPU 이득을 확정할 수 없다. 별도 isolate에서 notify와 merged를 직접 비교한 결과는 각각 18.065/17.951 FPS로 작고 실행마다 방향이 달랐다. 정렬 제거만의 추가 이득이라고 주장하지 않는다. 후속 비교는 전체 구성을 검증하기 위해 merged를 선택했다.

## 알림 진단

10초 별도 profile 실행이다. 시간 계측 및 진단 카운터 오버헤드가 있어 성능 평균에 포함하지 않는다.

| 항목 | base | fast | notify | merged |
|---|---:|---:|---:|---:|
| wake_workers 호출 | 5,744,367 | 5,876,587 | 1,055,443 | 1,091,126 |
| 유예한 게시 알림 | 0 | 0 | 5,136,164 | 5,390,621 |
| ticker 진입 flush | 0 | 0 | 31,507 | 32,033 |
| 정렬 생략 batch | 0 | 4,093,527 | 0 | 4,550,550 |

merged의 wake 호출은 base보다 약 81% 줄었다. 이 수치는 Worker 알림 경로 호출이지 커널 대기 해제 횟수가 아니다. 실행 완료 알림은 그대로 보내므로 wake 호출 전체가 flush 수까지 줄어드는 것은 아니다. 정렬 생략은 실제 동작하지만, 이 카운터만으로 FPS 기여도를 분리할 수 없다.

## 후속 교차 비교

각 20초, seed 42/43 평균이며 CPU는 전체 논리 프로세서 기준이다.

| Actor | 새 Area FPS / CPU | 앞선 묶음 게시 FPS / CPU | 이번 결합 후보 FPS / CPU |
|---|---:|---:|---:|
| 7만 | 19.999 / 24.21% | 19.997 / 36.24% | 19.995 / 29.01% |
| 14만 | 19.986 / 42.10% | 19.941 / 61.52% | 19.996 / 53.42% |
| 28만 | 19.871 / 83.28% | 16.557 / 87.75% | 18.218 / 88.04% |

28만에서는 같은 CPU 수준에서 약 10.0% 높은 평균 FPS를 기록했다. 7만/14만은 약 20 FPS를 유지하면서 CPU가 약 7.22%p/8.11%p 낮아졌다. 이전 보고서의 15.8 FPS와 이번 18.2 FPS를 직접 비교해 향상률을 계산하지 않는다. 이번 동일 실행 파일의 옵션 OFF/ON 교차 비교를 기준으로 한다.

| 28만 갱신 편차 | 앞선 묶음 게시 | 이번 결합 후보 |
|---|---:|---:|
| 두 실행 중 최저 Actor FPS | 15.409 | 16.272 |
| 두 실행 중 최대 갱신 간격 | 359ms | 190ms |

이 실행에서 최저 Actor FPS와 최대 갱신 간격은 개선됐지만, 평균과 최저 FPS의 차이가 항상 줄어든 것은 아니다. 두 seed만으로 모든 워크로드의 tail latency 개선을 보장하지 않는다. 새 Area는 여전히 평균 처리량과 CPU 효율에서 우세했다.

## 적용

legacy의 동일 bucket 경로와 알림 통합을 기본 ON으로 선택했다. 핵심 이득은 알림 통합에서 확인됐으며, 정렬 생략만의 FPS 이득은 확인 불가다. 두 기능은 별도로 끌 수 있다.

- AREAUPDATE_RADIUS_FAST_BUCKET=0: 항상 기존 정렬 경로를 사용한다.
- AREAUPDATE_RADIUS_MERGE_NOTIFY=0: 게시 알림을 즉시 보낸다.
- 두 변수를 모두 0: 이번 변경 전 묶음 게시 구성으로 비교한다.

타이머/게시 묶음은 앞선 변경대로 기본 ON, batch는 64, 진단 계측은 OFF다. 새 Area 및 Core는 변경하지 않았다. 긴 timer/request callback이 있는 실게임 부하에서는 알림 유예가 지연을 늘릴 수 있으므로 별도 측정 후 선택해야 한다.

## 경계 밀집

7만 Actor를 인접 2×2 Area 안에 두고 이동도 이 범위로 제한했다. 각 20초, seed 42/43 평균이다.

| 구성 | FPS | CPU | 두 실행 중 최대 갱신 간격 |
|---|---:|---:|---:|
| 새 Area | 20.006 | 2.89% | 118ms |
| base | 19.981 | 3.13% | 124ms |
| merged | 19.987 | 3.25% | 121ms |

보호 범위가 겹치는 제한된 배치에서는 처리량 이득이 없었다. CPU 차이도 작으며 이 결과를 넓은 맵 비교와 혼합하지 않는다.

## 최종 검증

- MSBuild Source/AreaUpdate.sln Release/Debug x64 빌드 성공. 최종 바이너리는 기본 Task 통계 ON으로 복구했다.
- Release/Debug AreaTests 통과. fast/merge 조합별 외부 게시 대기 경계, 부분 게시 후 마지막 Runner 대기, 혼합/동종 bucket FIFO, 복수 생산자, 자신의 Worker stop 이후 128개 작업 처리 및 pending flush 검사를 포함한다.
- UpdateComparison --test 통과. 기존 object/original 회귀 검사도 포함한다.
- 통계 ON, 5천 Actor: fast/merge 4개 조합, 게시/타이머 묶음/batch 16개 조합의 이동·이벤트·등록/탈퇴 검사 20회 통과.
- 경계 verify/synthetic × area/legacy × seed 42/43 검사 8회 통과. overlaps=0, sent=applied, tasks_live=0이었다. verify는 ownership_cycle=1이며 synthetic에서는 등록/탈퇴를 검사하지 않는다.
- 의도적인 경계 이벤트 ID 손상을 검증기가 거부하는 것도 확인했다.
- Format-Code.ps1 -All -Check, Check-CodeStyle.ps1, git diff --check 통과. C++의 기존 BOM과 CRLF를 유지했다.
- reference SHA256과 대조해 새 Area, 공통 Core, 원본 Baseline 파일이 변경되지 않았음을 확인했다.

통계 OFF 비교는 scout 8회, isolate 4회, profile 4회, final 18회, boundary 6회다. profile은 별도 진단이며 성능 평균에 섞지 않았다. 최종 비교의 미갱신 Actor는 모두 0이었다. 성능 바이너리에서는 옵션을 명시했고 기본값 변경은 최종 빌드에 반영했다. 실게임/장시간 운영 안정성 전체를 보장하는 검증은 아니다.

검증 로그는 artifacts/radius-fast/validation, 최종 소스 복사본은 final-source에 보관한다.
