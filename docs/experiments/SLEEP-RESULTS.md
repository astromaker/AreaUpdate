# Worker sleep_for 폴링 실험

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

## 변경

Worker는 할 일이 없을 때만 std::this_thread::sleep_for(1ms)를 호출한다.
남은 요청·만료 타이머·실행 가능한 Area가 있으면 바로 다음 루프를 돈다.
Windows 고해상도 타이머나 timeBeginPeriod는 추가하지 않았다.

Worker의 WakeSignal 멤버는 제거했다. wake()는 잠을 중단하지 않으며,
기존 AreaExecuter가 사용하는 유휴 Worker 표시 전이만 유지한다.
따라서 새 작업 도착과 종료 요청은 다음 폴링 때 반영된다.
TaskDispatcher의 조건 변수 대기와 round-robin 정책은 변경하지 않았다.

## 결과

2026-10-31, Release x64, Worker 8개, seed 42.
Actor 10,000개, 이동 시도 초당 2,000회, 10초 실행.
분산 입장은 첫 5초에 나누어 등록한다.

| 입장 | Windows 이전 측정 | 조건 변수 이전 측정 | sleep_for 3회 중앙값 |
|---|---:|---:|---:|
| 분산 Actor FPS | 18.808 | 16.005 | 15.812 |
| 일괄 Actor FPS | 18.728 | 15.904 | 15.696 |

sleep_for 버전의 갱신률은 Windows 기준 대비 각각 15.9%, 16.2% 낮았다.
sleep_for의 p99 갱신 간격 버킷은 두 시나리오 모두 70~79ms,
실행별 최대 타이머 지연의 중앙값은 모두 31ms였다.
조건 변수 버전 대비 갱신률 회복은 확인되지 않았다.

분산 입장 첫 실행의 Worker 대기 횟수 합계는 7,311회, idle 시간 합계는 78,384.704ms였다.
즉 기록된 대기 구간 평균은 약 10.7ms로, 요청한 1ms와 달랐다.
이는 이 환경에서 관찰한 값이며 모든 Windows 환경의 고정 해상도를 뜻하지 않는다.

## 재확인과 CPU

환경 변화 영향을 확인하기 위해 보존한 실행 파일을 조건 변수→Windows→sleep 순서로
각 시나리오 1회씩 추가 실행했다. 초기 6회와 재확인 6회, 총 12회다.
재확인 원본 값은 artifacts/std-port 각 단계의 sleepcheck-runs.csv에 있다.

| 버전 | 분산 FPS | 일괄 FPS | 분산 CPU 초 | 일괄 CPU 초 |
|---|---:|---:|---:|---:|
| 조건 변수 | 15.678 | 15.790 | 2.844 | 3.703 |
| Windows | 18.792 | 18.749 | 2.469 | 3.156 |
| sleep_for | 15.916 | 15.659 | 1.750 | 2.078 |

이 표는 각 1회의 재확인 값으로, CPU 차이의 확정적인 비율을 뜻하지 않는다.

프로세스 전체 CPU 시간도 새로 수집했다. 이는 시작·맵 준비·종료를 포함한
모든 스레드의 CPU 시간 합계이며 기존 벤치마크의 측정 구간 CPU 시간과 범위가 다르다.
초기 sleep 3회 CPU 시간 중앙값은 분산 1.953초, 일괄 2.344초였다.
갱신 횟수 자체가 감소했으므로 CPU 시간이 낮다는 이유만으로 효율 향상이라고 해석하면 안 된다.

## 검증

Release/Debug 빌드, AreaTests, UpdateComparison --test와 서식·명명 검사를 통과했다.
이동 예제의 overlaps, never_updated, tasks_live, tasks_pending이 모두 0인지 확인했다.
이번 변경은 Worker 대기 실험이며 TaskExecuter 처리량 벤치마크를 다시 측정하지 않았다.

현재 소스는 sleep_for 버전이다. 조건 변수 Worker 소스는
artifacts/std-port/condition-source에, 이전 실행 파일은 std 폴더에 보존했다.
sleep 폴더의 실행 파일·로그·runs.csv가 이번 측정 대상이다.
Measure-StdPort.ps1 -Stage sleep -RuntimeOnly -RuntimeActors 10000으로 당시 인원 조건을 지정할 수 있다.
후속 기본값은 70,000개이며 다른 인원의 측정 파일에는 별도 접두사를 사용한다.

결론: 단순 sleep_for(1ms) 폴링도 이 환경에서 기존 고해상도 타이머의 갱신률을 회복하지 못했다.
