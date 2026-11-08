# Worker 깨우기와 sleep 비교

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

이 문서는 당시의 비교 기록이다. 후속 Area 진단 후 현재 기본값은 signal로 변경했다.
최신 기본 설정과 두 모델 비교는 SIGNAL-MODEL-COMPARISON.md를 참고한다.

## 변경 범위

Core Worker에 sleep_for(1ms) 대신 WakeSignal로 대기하는 비교 옵션을 추가했다.
WakeSignal은 std::mutex와 std::condition_variable로 알림을 보관·병합한다.
Windows 이벤트·고해상도 타이머는 사용하지 않는다.

비교에서 처리량 개선을 확인하지 못해 최종 기본값은 기존 poll로 유지했다.
Worker::set_polling_wait(false) 또는 예제의 마지막 인수 signal로 새 경로를 선택한다.

- 즉시 요청·종료·Area 스케줄러 알림은 대기 중인 Worker를 깨운다.
- 타이머는 현재 기다리는 만료보다 이른 요청만 깨운다.
- 더 늦은 예약 요청은 이미 정해진 기상 시점에 일괄 등록한다.
- 예약이 없고 외부 작업 알림이 보장되면 무기한 기다린다.
- 무알림 Ticker는 최대 1ms 제한 대기를 사용한다. 예전 Area 어댑터가 여기에 해당한다.
- 대기 시각 공개 → 대기 상태 공개 → 큐 재확인 → 보관된 알림 소비 순서로 유실을 방지한다.

5ms·256슬롯, 요청/타이머 batch 256, Area batch 64와 partition 정책은 유지했다.
TaskExecuter의 Actor 직렬화, Dispatcher의 round-robin과 재전달 정책은 변경하지 않았다.
타이머 Worker 8개와 Dispatcher Worker 8개도 통합하지 않았다.
오브젝트 풀·idle Worker 관리·stealing은 추가하지 않았다.

## 비교 조건

같은 Release 실행 파일에서 마지막 인수만 poll 또는 signal로 바꾼다.
poll은 기존 sleep_for(1ms), signal은 새 조건 변수 대기다.
두 모드 모두 동일한 통계·타이머 지연 추적 코드를 포함한다.

Object(job) 모드, 목표 20FPS, 고정 50ms 예약, 추가 연산 0으로 측정한다.
Actor 1만·7만·14만·28만·56만, 밀도 유지를 위해 맵 한 변을
ceil(320 × sqrt(Actor 수 / 70000))으로 늘린다.
5,000ms 분산 입장과 1ms 입장 확인, seed 42를 사용한다.
이동 시도는 ceil(Actor 수 × 2000 / 70000)회/초다.
각 20초씩 2회, 첫 회 poll→signal, 다음 회 signal→poll 순서로 실행한다.
CPU는 입장 완료 후 1초 이후 표본의 시간 가중 평균이며 전체 논리 CPU 기준이다.
FPS와 타이머 지연 분포는 입장 구간을 포함한 실행 전체 기준이다.
이번 예제에는 Actor 간 상호작용 부하가 없으므로 실제 게임 성능으로 일반화하지 않는다.

## 첫 실험의 문제

artifacts/wakeup-comparison에는 모든 타이머 등록 요청이 대기를 깨우던 첫 버전을 보존했다.
7만 Actor 첫 signal 실행에서 약 491만 번의 조건 변수 대기가 발생했고,
FPS 14.242·CPU 80.52%였다. 같은 회 poll은 FPS 17.652·CPU 71.29%였다.
단순히 sleep을 알림으로 바꾸기만 하면 알림 빈도가 새로운 비용이 될 수 있다.
이 결과를 확인한 뒤 첫 캠페인을 중단하고 미래 타이머 알림을 생략하는 정책을 추가했다.
첫 버전의 일부 결과와 개선판의 결과를 섞어 집계하지 않는다.

## 비교 결과

개선판의 추적 활성 시험은 1만~28만에서 16회 완료했다.
56만은 실행 직전 가용 메모리 안전 예산에 걸려 제외했고 강행하지 않았다.
해당 캠페인은 안전 예산 예외로 중단됐으며, 이미 완료된 16회 결과는 보존했다.
재현 스크립트는 이후 안전 예산 제외를 plan.csv에 기록하고 정상적으로 중단하도록 보완했다.

아래 값은 각 조건 20초 2회 지표의 중앙값이다. 두 값의 중앙값은 산술평균과 같다.
반복 수가 적고 전용 측정 장비가 아니므로 작은 차이를 확정적 우열로 해석하지 않는다.

### 타이머 지연 추적 활성

| Actor | poll FPS | signal FPS | poll CPU | signal CPU | poll 지연 p99 | signal 지연 p99 |
|---:|---:|---:|---:|---:|---:|---:|
| 10,000 | 20.00 | 19.94 | 33.57% | 33.67% | 21ms | 23.5ms |
| 70,000 | 18.24 | 16.71 | 83.10% | 75.43% | 69.5ms | 82ms |
| 140,000 | 14.50 | 12.65 | 90.40% | 80.48% | 72ms | 103.5ms |
| 280,000 | 7.30 | 7.35 | 88.46% | 89.34% | 84ms | 74.5ms |

지연을 추적하는 상태에서는 7만·14만의 signal 처리량이 낮았다.
타이머마다 timestamp·histogram을 기록하며 큰 지연마다 상세 기록도 구성하므로,
추적 비용의 영향을 구분하기 위해 7만·14만·28만에서 추적 비활성 대조를 추가했다.

### 타이머 지연 추적 비활성

추가 12회도 각 조건 20초 2회이며 같은 실행 파일에서 추적 인수만 0으로 변경했다.
기존 기본 통계와 Actor별 계측까지 제거한 순수 엔진 빌드는 아니다.

| Actor | poll FPS | signal FPS | poll CPU | signal CPU |
|---:|---:|---:|---:|---:|
| 70,000 | 18.87 | 18.65 | 87.09% | 86.83% |
| 140,000 | 15.06 | 14.70 | 91.79% | 88.88% |
| 280,000 | 6.56 | 6.86 | 82.50% | 80.99% |

28만 poll의 FPS는 5.802와 7.317로 편차가 컸다. signal은 6.794와 6.923이었다.
따라서 중앙값의 약 4.6% 차이를 안정적 개선이라고 확정할 수 없다.
7만·14만도 signal이 목표 20FPS를 회복시키지 못했다.
추적 활성/비활성은 별도 실험이므로 차이 전체를 추적 오버헤드라고 단정하지 않는다.

### 판단

새 요청이 생긴 Worker를 깨우는 기능과 미래 타이머 알림 병합은 동작한다.
하지만 이 시험에서는 일관된 FPS·CPU 개선을 확인하지 못했다.
기본값은 기존 poll로 유지하고 signal은 비교 옵션으로 보존했다.
sleep을 바꾸면 현재 처리량 한계를 해결할 수 있다는 가설은 이번 결과로 지지되지 않는다.
단, 모든 대기 지연이 무의미하다거나 다른 게임 부하에서도 효과가 없다는 결론은 아니다.

추적 활성 상태의 Core Worker 최대/평균 busy 시간 비율은 조건별 중앙값 약 1.02~1.08이었다.
이는 큰 Core Worker 불균형을 보여주지는 않지만, 벽시계 시간이며 Dispatcher의 Worker별
실행 시간은 여기 포함되지 않아 Actor 실행 부하 분산까지 검증했다고 볼 수 없다.

## 큰 지연 추적

14만 signal 두 번째 실행에서 224ms 사건을 보관했다.
만료 이후 등록 요청 큐 대기 118ms와 등록 후 선택 polling까지 106ms로 분해됐다.
마지막 조건 변수 대기가 만료 이후 요청 큐 대기와 겹친 시간은 116ms였고,
직전 요청 처리 구간의 벽시계 시간은 106ms, 요청 잔량은 15,769였다.
대기 알림이 있다는 것만으로 OS의 적시 실행이나 적체된 등록 요청의 즉시 처리가 보장되지는 않는다.
이 기록만으로 116ms가 OS 스케줄링·실제 대기·잠금 대기 중 어느 비용인지 확정할 수 없다.
106ms도 순수 CPU 시간이 아니다. 해당 사건은 과거의 128ms 사건과 별개다.

다음 분석 후보는 Dispatcher 제출부터 Actor 실행 시작까지의 대기와,
타이머 callback/등록 요청 처리에 쓰인 CPU 시간·잠금 대기·스케줄링 지연의 구분이다.
현재 자료만으로 특정 mutex나 자료구조가 주원인이라고 단정하지 않는다.

## 지표 해석

- 타이머 지연은 Core Worker의 callback 실행 직전까지이며 Actor 실행 시작까지의 전체 지연이 아니다.
- wake_latency는 최초 보관된 알림 기록부터 소비까지의 벽시계 시간이다.
  실제 대기 이전에 보관된 알림의 시간도 포함하므로 순수 OS 깨우기 지연이라고 단정하지 않는다.
- 미래 타이머의 요청 큐 대기는 의도적으로 길어질 수 있다. 만료 이후 지연과 구별해야 한다.
- 기존 SLEEP 및 타이머 추적의 sleep 시각 필드는 signal 모드에서는 조건 변수 대기를 뜻한다.
- overshoot 총합은 대기 목표 자체가 달라 모드 간 직접 비교하지 않는다.
- 상세 타이머 추적은 Worker별 큰 지연 32건으로 제한돼 전체 원인 비율을 추정하는 표본이 아니다.

## 재현

```powershell
.\tools\experiments\Run-WakeupComparison.ps1 -OutputDirectory artifacts/new-wakeup-run
# 성능 판단용 추적 비활성 비교
.\tools\experiments\Run-WakeupComparison.ps1 -ActorCounts 70000,140000,280000 -TimerTraceThresholdMs 0 -OutputDirectory artifacts/new-wakeup-off
# 마지막 인수만 변경해 개별 실행
.\Source\x64\Release\AreaUpdateExample.exe 20 70000 2000 42 5000 1 0 320 job 50 signal
.\Source\x64\Release\AreaUpdateExample.exe 20 70000 2000 42 5000 1 0 320 job 50 poll
```

tools/experiments/Run-WakeupComparison.ps1은 실행 파일·해시·Core 소스를 보존하고,
실행마다 로그·CPU 표본·요약·Worker 통계와 임계값 이상의 타이머 지연 상세 기록을 저장한다.
중복 실행·미갱신 Actor·잔여 Task를 검증하며 최대 타이머 지연의 상세 보관 여부도 확인한다.

## 자료와 검증

- artifacts/wakeup-deadline-comparison/results.csv: 추적 활성 16회.
- trace-off-results.csv: 추적 비활성 12회. 비활성 histogram은 null이며 0ms가 아니다.
- aggregate.csv / trace-off-aggregate.csv: 조건별 중앙값과 범위 요약.
- 각 실행의 runtime.log, samples.csv, summary.json, 상세 추적 CSV.
- trace-analysis.csv: 보관된 지연 타임라인 분해 검증.
- 측정 실행 파일 SHA-256: 480BDAC1128B717F225246FDE8AEE300802A833B37A1E3F31956B656898F710C.

최종 소스는 비교 이후 기본값만 poll로 복원했다. 측정 시에는 모든 실행에서 모드를 명시했다.
보존된 실행 파일의 인수 생략 기본값은 signal이며, 새로 빌드하는 예제의 기본값은 poll이다.
추적 활성 16회와 비활성 12회 모두 종료 코드 0, 중복·미갱신 Actor·잔여 Task 0이었다.
유실 방지, 더 이른 타이머, 더 늦은 타이머 알림 생략, 무알림 Ticker fallback,
긴 타이머 시각 변환 및 중단·소유권 회귀 테스트를 추가하거나 유지했다.
MSBuild Release/Debug 솔루션 빌드, AreaTests 각 5회 반복, area/original/job의
poll/signal 6개 조합 smoke 테스트와 서식·명명·초기화·중괄호 검사를 통과했다.
