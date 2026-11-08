# Signal 기본 대기 적용 후 Area와 Object 비교

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

## 기본값 변경

- Core Worker의 m_pollingWait 기본값을 false로 변경했다.
- 예제의 waitMode 기본값을 signal로 변경했다.
- tools/benchmark/Measure-RuntimeCpu.ps1의 WaitMode 기본값도 signal로 변경했다.
- set_polling_wait(true) 또는 예제의 poll 인수로 이전 방식을 명시할 수 있다.

기존 신호 대기 구현을 기본 선택으로 바꾼 것이며 partition·반경·batch 64·admission 8·공평성은 변경하지 않았다.
Actor 소유권, TaskExecuter 직렬 실행, Dispatcher의 round-robin·재전달도 그대로다.
Area와 Object 둘 다 signal로 비교한다. Area만 signal이고 Object는 poll인 결과를 섞지 않는다.
TaskDispatcher 자체의 대기는 원래부터 WakeSignal을 사용했으며 이번 변경은 Core Worker의 기본값이다.
알림을 보장하지 않는 Ticker는 기존 1ms 제한시간 재확인을 유지한다. 실제 기상 시각의 상한 보장은 아니다.

## 조건

같은 Release 실행 파일에서 area/job 모드를 바꾸며 각 조건 20초씩 3회 실행한다.
첫 회 Area→Object, 다음 회 Object→Area, 세 번째 회 Area→Object다.
tools/experiments/Run-SignalModelComparison.ps1은 WaitMode를 생략해 측정 도구의 새 기본값을 사용하고,
실제 출력의 wait_mode=signal과 signal_waits>0을 검증한다.
예제 자체도 waitMode를 생략한 기본 실행으로 별도 확인한다.

| Actor | 맵 한 변 | 이동 시도/초 |
|---:|---:|---:|
| 10,000 | 121 | 286 |
| 70,000 | 320 | 2,000 |
| 140,000 | 453 | 4,000 |
| 280,000 | 640 | 8,000 |

5초 분산 입장·1ms 입장 확인·seed 42·추가 Actor 연산 0이다.
두 모델 모두 예정 시각 기준 고정 50ms 예약이며 늦으면 지난 tick을 건너뛴다.
타이머는 5ms·256슬롯, Task 생성/소멸 통계는 ON이다.
상세 타이머 추적·Actor 제출 표본·Dispatcher 프로파일은 모두 OFF다.
Area는 Core Worker 8개, Object는 Core Worker 8개와 Dispatcher Worker 8개를 사용한다.
CPU는 전체 논리 CPU 8개 기준, 입장 완료 후 1초 이후 표본의 시간 가중 평균이다.
FPS는 각 Actor 입장 이후 시간 합계를 분모로 한 전체 실행의 평균 Actor FPS다.
실행별 FPS와 CPU의 중앙값을 사용하며 전체 실행을 합친 분포의 분위수는 아니다.

## 결과

총 24회 실행이 완료됐다. 아래는 조건별 3회 중앙값이다. CPU 100%는 논리 CPU 8개 전체 사용을 뜻한다.

| Actor | Area FPS | Object FPS | Area CPU | Object CPU |
|---:|---:|---:|---:|---:|
| 10,000 | 19.998 | 20.020 | 4.04% | 30.33% |
| 70,000 | 20.006 | 18.321 | 26.61% | 80.72% |
| 140,000 | 19.912 | 17.987 | 50.89% | 92.88% |
| 280,000 | 11.591 | 8.970 | 61.57% | 91.82% |

| Actor | Area FPS 범위 | Object FPS 범위 | Area CPU 범위 | Object CPU 범위 |
|---:|---:|---:|---:|---:|
| 10,000 | 19.998~19.999 | 20.008~20.021 | 3.87~4.27% | 29.26~30.36% |
| 70,000 | 19.999~20.007 | 17.409~19.653 | 26.34~26.71% | 71.93~88.20% |
| 140,000 | 19.805~19.954 | 17.940~18.036 | 50.87~55.51% | 92.54~92.98% |
| 280,000 | 11.441~11.717 | 8.895~9.036 | 60.86~62.46% | 90.61~92.31% |

이 조건에서는 Area가 7만까지 평균 약 20 FPS를 유지하고 14만에서도 목표에 근접했다.
Object보다 CPU 사용량도 낮았다. 7만 Object는 반복 간 편차가 크므로 단일 실행으로 판단하지 않는다.
28만에서는 두 모델 모두 목표 미달이다. Area는 CPU 여유가 남아도 목표를 달성하지 못하므로,
signal 변경만으로 partition admission과 실행 장벽 등 남은 제약이 제거됐다고 볼 수 없다.
반대로 목표 FPS를 달성한 저부하 구간의 낮은 CPU는 문제의 증거가 아니다.

측정 실행 파일 SHA-256: `486C9BFC26E82B7EBD4D0EAD7A65FC9B42A78C23D6D020DC707E91B6B7D0F3B4`.

## 해석 범위

균일한 가벼운 갱신 예제이며 빈번한 Actor 간 상호작용 부하가 없다.
Area는 목록 단위 갱신과 pending 소유권 이동을 사용하고 Object는 Actor별 실행기/예약을 사용한다.
Area로 이동한 Actor는 목적지 예약 위상에 영향을 받는다. 모든 Actor의 간격이 항상 50ms인 것은 아니다.
Object도 공통 Area 그리드를 할당하므로 최소 메모리 구현끼리의 비교는 아니다.
평균 FPS가 20에 가깝다는 것이 모든 Actor의 모든 deadline을 지켰다는 뜻은 아니다.
작은 차이를 일반적인 우열이나 실제 게임 서버 전체 성능으로 확정하지 않는다.

## 검증과 재현

기본 Worker의 interruptible_wait 회귀 검사에서 명시적인 설정을 제거하고,
기본 상태에서 signalWaits/notifiedWaits가 기록되는지 확인한다.
예제의 Area/Object도 waitMode 인수를 생략해 실제 signal 경로 사용을 확인한다.
명시적 poll, 무알림 Ticker fallback, 고정 주기·재활성화·소유권·공평성·round-robin 검사는 유지한다.

```powershell
# 기본값을 사용: Area와 Object 모두 signal
.\Source\x64\Release\AreaUpdateExample.exe 20 140000 4000 42 5000 1 0 453 area
.\Source\x64\Release\AreaUpdateExample.exe 20 140000 4000 42 5000 1 0 453 job
# 이전 대기 방식을 명시
.\Source\x64\Release\AreaUpdateExample.exe 20 140000 4000 42 5000 1 0 453 area 0 poll
# 보존된 실행 파일로 새 결과 폴더에 재실행
.\tools\experiments\Run-SignalModelComparison.ps1 -OutputDirectory artifacts/signal-default-repeat
```

artifacts/signal-default-comparison/after에 측정 실행 파일과 변경한 핵심 파일을 보존한다.
measurements에는 환경·SHA-256·메모리 안전 계획·실행별 로그·CPU 표본·요약·results.csv를 보존한다.
tests에는 인수 생략 기본값 검사와 회귀 검사 자료를 보관한다.
과거 poll 비교 자료는 덮어쓰거나 기본값 변경 후 수치로 재해석하지 않는다.
