# 표준 C++ 전환 측정 결과

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

## 결론

처리량 자체의 일괄적인 큰 하락은 확인되지 않았다. 그러나 50ms 주기 이동 예제에서는
조건 변수 기반 대기·타이머 전환 후 Actor 갱신률이 약 15% 낮아졌다.
따라서 현재 구현을 Windows 고해상도 타이머의 성능 동등 대체라고 볼 수 없다.
Windows에서 기존 갱신 정밀도를 우선한다면 고해상도 타이머 백엔드를 유지하는 편이 적합하다.
이 문서는 조건 변수 전환 시점의 결과다. 후속으로 Worker를 sleep_for(1ms)로 변경했으며
현재 소스와 그 비교는 SLEEP-RESULTS.md를 참고한다. Windows 원본 실행 파일과 소스는 artifacts에 보존했다.

## 변경 범위

| 단계 | 큐와 Task | 대기·타이머 |
|---|---|---|
| windows | Windows SList, SList 기반 Task 정렬·상속 | 이벤트 + 고해상도 waitable timer |
| queue | 기존 std::atomic TaskQueue로 통합, SList 상속 제거 | Windows 방식 유지 |
| std | queue 단계와 동일 | mutex + condition_variable, steady_clock wait_until |

Core/ObjectUpdate의 직접 Windows API 사용과 WindowsHandle을 제거했다.
WakeSignal은 신호를 mutex로 보호해 보관·병합하므로 대기 진입 전 알림도 잃지 않는다.
Worker/TaskDispatcher의 waiting 공개 후 재검사, round-robin 배정, Actor별 직렬화,
한 batch 처리 후 재전달, partition 공평성·batch 64는 유지했다.

원본 스냅샷은 수정하지 않았다. 벤치마크의 GetProcessTimes CPU 측정,
Visual Studio 프로젝트·측정 스크립트는 Windows 전용으로 남아 있다.
따라서 전체 저장소의 다른 OS 빌드가 완료됐다는 뜻은 아니다.

## 측정 조건

- 2026-10-31, Windows 10.0.26200, 논리 프로세서 8개, Worker 8개.
- MSBuild Source/AreaUpdate.sln, Release x64, seed 42.
- 최초 측정: 3단계 × 6시나리오 × 3회 = 54회.
- 추가 처리량 비교: windows→queue→std, queue→std→windows, std→windows→queue 순서.
  각 순서에서 4시나리오를 실행해 총 36회. 아래 처리량 표는 이 추가 비교의 중앙값이다.
- 추가 런타임 확인: std→windows→queue 순서로 분산·일괄 입장을 각 1회 재실행.
- 독점 장비가 아니며 CPU 고정·전력 상태 통제는 하지 않았다. 반복 범위도 함께 확인해야 한다.

## 처리량

동일한 고정 작업량을 완료한 시간(초), 낮을수록 좋다.
각 실행은 워밍업 5 tick 후 새 월드에서 측정했다.

| 시나리오 | Actor / tick / 상호작용 / work | Windows | 큐만 변경 | 전체 std | std 처리량 변화 |
|---|---|---:|---:|---:|---:|
| cluster-light | 5000 / 100 / 8 / 1000 | 1.886 | 1.839 | 1.829 | +3.1% |
| cluster-interaction | 5000 / 100 / 32 / 1000 | 4.416 | 4.267 | 4.621 | -4.4% |
| cluster-heavy | 5000 / 40 / 8 / 20000 | 1.969 | 1.740 | 1.928 | +2.1% |
| hotspot | 2000 / 100 / 32 / 1000 | 1.919 | 1.979 | 1.741 | +10.2% |

처리량 변화는 updates_per_sec 중앙값을 Windows 값과 비교했다.
단계별 3회 표본이며, 고정적인 개선·회귀 비율이나 통계적 유의성을 주장하지 않는다.
예를 들어 interaction의 실행 범위는 Windows 4.257~4.962초, std 4.135~5.092초로 겹친다.
최초 순차 측정에서는 hotspot 등에서 다른 크기의 차이도 나타났으므로,
실행 순서를 바꾼 추가 비교를 최종 처리량 표로 사용했다. 최초 결과도 삭제하지 않았다.

같은 추가 비교에서 프로세스 CPU 시간 중앙값은 다음과 같다. 단위는 모든 스레드 합계 초다.

| 시나리오 | Windows | 큐만 변경 | 전체 std |
|---|---:|---:|---:|
| cluster-light | 6.891 | 6.750 | 7.109 |
| cluster-interaction | 17.281 | 15.672 | 17.422 |
| cluster-heavy | 9.359 | 9.281 | 9.078 |
| hotspot | 7.188 | 6.016 | 7.016 |

이 비교는 TaskDispatcher/TaskExecuter 경로이며 Worker의 예약 타이머 성능을 검증하지 않는다.

## 50ms 주기 런타임

Actor 10,000개, 이동 시도 초당 2,000회, 실행 10초, 320×320 Area.
분산 입장은 첫 5초 동안 추가하고, 일괄 입장은 시작 때 모두 등록한다.
각 Actor의 입장 이후 시간 합계를 분모로 계산한 actor_fps를 사용했다.
표는 최초 3회 실행의 중앙값이다.

| 입장 방식 | 지표 | Windows | 큐만 변경 | 전체 std |
|---|---|---:|---:|---:|
| 분산 | Actor FPS | 18.808 | 18.788 | 16.005 |
| 일괄 | Actor FPS | 18.728 | 18.753 | 15.904 |
| 분산 | p99 갱신 간격 버킷 | 50ms대 | 50ms대 | 70ms대 |
| 일괄 | p99 갱신 간격 버킷 | 50ms대 | 50ms대 | 70ms대 |
| 분산 | 최대 타이머 지연 | 9ms | 7ms | 33ms |
| 일괄 | 최대 타이머 지연 | 9ms | 8ms | 30ms |

최대 타이머 지연은 각 실행에서 측정한 최대값 3개의 중앙값이지 p99가 아니다.
갱신 간격은 10ms 단위 히스토그램이며 50ms대는 50~59ms를 뜻한다.
std 버전의 갱신률 손해는 분산 14.9%, 일괄 15.1%다.
추가 교차 실행에서도 분산/일괄 FPS는 Windows 18.820/18.765,
queue 18.796/18.771, std 16.034/15.987로 같은 방향의 하락을 재확인했다.
추가 실행에서 std의 p99 간격 버킷은 분산 60ms대, 일괄 70ms대였다.
런타임의 busy_ms는 애플리케이션 실행 구간의 벽시계 시간이며 프로세스 CPU 시간이 아니다.
이 실험에서는 런타임의 실제 CPU 시간은 수집하지 않았다.

큐만 변경한 단계에서는 이 갱신률 하락이 없었다. 대기·타이머 변경 단계에서 발생한 차이는
확인했지만, 조건 변수 비용과 OS 타이머 해상도 등 커널 내부 원인의 개별 기여도는 확인하지 못했다.
현재 Area 재예약은 완료 시점 + 50ms라 추가 지연이 다음 주기에 누적될 수 있다.

## 검증과 자료

- Release/Debug 빌드, AreaTests, UpdateComparison --test 통과.
- 신호 선행·병합·시간 만료·1,000회 handshake 검증 추가.
- 큐 FIFO, 다중 생산자, Actor 직렬화, 제출·종료 경합 검증 유지.
- 측정 결과의 체크섬 일치, overlaps/never_updated/tasks_live/tasks_pending=0 확인.
- 최초 54회 + 순서 순환 36회 + 런타임 재확인 6회, 총 96회 결과 검증 완료.
- tools/quality/Format-Code.ps1 -All -Check, tools/quality/Check-CodeStyle.ps1 통과.

자료는 artifacts/std-port 아래에 저장했다.

- windows / queue / std: 단계별 실행 파일, 원본 로그, runs.csv.
- paired*-runs.csv / paired-summary.csv: 실행 순서를 순환한 처리량 결과.
- runtimecheck-runs.csv: 추가 런타임 확인.
- summary.csv: 최초 측정의 중앙값과 범위.
- binaries.json / environment.json: 실행 파일 SHA-256, 측정 환경.
- before-source / queue-source: 전환 전·중간 단계 소스 보존.

재측정은 tools/experiments/Measure-StdPort.ps1, 집계는 tools/analysis/Summarize-StdPort.ps1을 사용한다.
현재 소스를 빌드해도 보존된 단계별 실행 파일은 자동 교체되지 않는다.
해당 실행 파일을 변경하면 다른 측정 세션으로 분리하고 로그와 해시도 다시 보존해야 한다.
