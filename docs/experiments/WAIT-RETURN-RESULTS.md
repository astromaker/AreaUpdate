# Object 타이머 Worker 복귀 지연 확인

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

## 결론

7만 Object에서 기준·전달 Task 제거 후보의 대기 복귀를 나눠 측정했다.
늦은 timer 복귀의 **기준 98.38%, 후보 99.45%가 timeout 판정**에 속했다.
유한 대기의 신호 요청 → 관측 복귀 평균은 약 13.09/12.41µs로,
이번에 증가한 기한 초과 누적 시간의 주요 부분은 신호 경로가 아니라 timeout 쪽이었다.

후보의 timeout 복귀 횟수는 약 2.1배였고, 요청한 약 2ms를 평균 약 10ms 초과해 돌아왔다.
이는 `condition_variable`의 실제 kernel timeout만 측정한 것이 아니라,
대기 호출 주변 코드·mutex·OS 재실행 배정까지 포함한 관측 복귀 지연이다.
이 중 어느 부분이 원인인지는 아직 분리하지 못했다.

## 조건

- C++14 설정 유지, `Source/AreaUpdate.sln` MSBuild Release x64.
- Actor 70,000, 맵 320, 이동 시도 초당 2,000, seed 42/43, 1초/1ms 분산 입장.
- timer Worker 8개 + Dispatcher 8개, 논리 CPU 8개.
- signal 대기, 고정 50ms 주기, 5ms × 256 휠, Task·Worker 기존 통계 ON.
- WPR OFF. ObjectSampleStride=256, DispatcherProfileStride=0.
- 각 20초, 기준/후보 × 복귀 계측 OFF/ON × 두 seed = 8회.
- seed 42: 기준 OFF→ON→후보 OFF→ON, seed 43은 역순.
- 같은 복귀 계측을 포함해 두 버전을 새로 빌드했다. 과거 보관 바이너리의 절대 FPS와 혼합하지 않는다.
- 실행 중 빌드·정적 검사·프로파일 추출을 병행하지 않았다.
- 전달 Task 제거 후보는 실험 빌드 때만 적용하고 기존 기본 소스를 복원했다.
- 예약 시각·대기 방식·round-robin·갱신 누락 정책은 변경하지 않았다.

## 계측 영향 대조

두 seed 정상 구간의 산술 평균이다. 복귀 계측 OFF에도 공통 Task/Worker 통계와 Actor 표본은 남아 있다.

| 모델 | 계측 OFF FPS / CPU | 계측 ON FPS / CPU |
|---|---:|---:|
| 기준 | 19.55 / 86.73% | 19.59 / 86.86% |
| 후보 | 18.86 / 76.37% | 18.88 / 76.78% |

이번 두 쌍에서는 계측으로 큰 FPS 변화가 생기지 않았고 후보의 회귀도 같은 방향이었다.
그렇다고 계측 부담이 0이거나 모든 환경에서 무시 가능하다고 확정하지 않는다.
Actor 표본 타이머 지연 P99도 OFF의 기준 55~63ms / 후보 74~78ms,
ON의 기준 52~61ms / 후보 69~79ms로 같은 양상이었다.

## 복귀 사유별 결과

횟수·누적 시간은 실행 1회당 두 seed 평균이다. 평균 지연은 두 실행의 합계/건수로 가중 계산했다.
아래는 계측 ON 결과다.

| 항목 | 기준 | 후보 |
|---|---:|---:|
| timeout 복귀 횟수 | 3,024.5 | 6,257.5 |
| timeout 중 timer 기한 초과 횟수 | 2,891.5 | 6,013 |
| timeout의 timer 기한 초과 누적 | 23,199ms | 58,909ms |
| timeout의 예정 대기 길이 평균 | 1.86ms | 2.10ms |
| timeout 복귀의 예정 시각 초과 평균 | 8.17ms | 9.91ms |
| timeout 초과 P99 bucket | 19~25ms | 18ms |
| 유한 신호 복귀 횟수 | 58,134.5 | 64,614.5 |
| 유한 신호 복귀 중 timer 기한 초과 횟수 | 47.5 | 33.5 |
| 유한 신호 복귀의 timer 기한 초과 누적 | 135.5ms | 61ms |
| 신호 요청 → 관측 복귀 평균 | 13.09µs | 12.41µs |

후보가 timeout 한 번의 가장 큰 지연이나 P99까지 더 나빴던 것은 아니다.
기준 timeout 최대 초과는 두 회차 54.53/42.37ms, 후보는 32.36/33.34ms였다.
차이는 **timeout 복귀 빈도와 평균 초과 누적**에서 나타났다.
후보에서 증가한 기한 초과 누적 시간은 사실상 timeout 증가로 설명되며 신호 쪽 누적은 오히려 감소했다.
timeout 복귀 중 계측 시작 시점에 이미 기한이 지난 경우는 두 실행 합계 각 버전 4회로 적었다.

누적 시간은 8개 Core Worker 합계다. 후보가 한 번에 약 59초 멈췄다는 뜻이 아니다.

## 사유 분류와 시간의 의미

`Source/Core/Worker.cpp::wait_for_work()`에서 기존 `WakeSignal::wait_until()` 반환값을 보존했다.

- timeout: predicate가 false로 반환한 유한 대기.
- signal: predicate가 true로 반환한 유한 대기.
- untimed: 기한 없는 신호 대기.
- poll: 기존 sleep_for 대기. 이번 signal 비교에서는 0회.

이 분류는 관측 반환 상태이지 kernel이 실제로 스레드를 깨운 원인을 직접 알려주는 것은 아니다.
예를 들어 기한이 지난 뒤 신호가 들어와 predicate 확인 전에 관측되면 signal로 분류될 수 있다.
이를 구분하려고 늦은 신호 요청, 기한 전 요청이지만 기한 후 복귀한 경우, 종료 중 복귀도 별도로 기록했다.

시간 구분:

- `late_timer_ms`: 기존 ms 시계의 `now > timer deadline`과 동일한 계산.
- `overshoot_ns`: 기존 Clock 관측 복귀 시각이 wait deadline을 넘긴 양. 1ms bucket histogram 사용.
  P99 bucket 0은 지연 없음이 아니라 1ms 미만 구간을 포함한다. 마지막 bucket 100은 100ms 이상이다.
- `notification_ns`: Worker의 신호 요청 시각부터 관측 복귀까지. 실제 OS notify 호출 시각은 아니다.
  producer의 WakeSignal mutex 획득, notify, CPU 배정, consumer mutex 재획득 등이 섞일 수 있다.
- `past_due_at_begin`: 기존 begin 시계에서 wait deadline이 이미 지났는지 기록.
  begin은 OS 대기 호출 바로 직전의 별도 시각이 아니므로 이후 선점을 배제하지 못한다.

새 Clock 조회나 전역 atomic 카운터는 추가하지 않았다.
기존 latency/duration용 Clock 관측값을 재사용하고 Worker 소유의 별도 블록에 기록했다.
신호 복귀 관측은 기존 latency 측정 지점, timeout은 기존 duration 측정 지점이다.
따라서 두 경로의 ns 수치를 순수한 OS timeout 지연처럼 해석하지 않는다.
기존 총 overshoot는 그보다 뒤의 duration 지점을 사용하므로 새 bucket 합과 반드시 같은 ns 값은 아니다.
timer 기한 초과 횟수/ms 합계와 notification 건수는 기존 카운터와 정확히 일치하도록 검사했다.

## 다음 확인 대상

후속으로 짧은 기한의 능동 대기를 실험했으나 이득이 제한적이어서 기본 코드에 채택하지 않았다.
`ACTIVE-WAIT-RESULTS.md`에 0.25/0.5/1/2ms 비교와 보관 자료를 정리했다.

이제 우선 확인할 경로는 **약 2ms 남은 deadline을 유한 wait_until로 기다리는 부분**이다.
신호 전달 Task를 더 줄이거나 신호를 무조건 많이 보내는 변경이 이번 지연의 직접 해결책이라는 근거는 없다.

다음 개선 실험은 짧은 기한의 대기를 별도로 취급하는 정책을 독립 비교하는 것이 적절하다.
CPU 비용과 FPS·갱신 편차를 함께 봐야 하며, 이번에는 실제 대기 정책을 바꾸지 않았다.
OS timer 해상도·실행 재배정·내부 mutex 비용 중 어느 부분이 주원인인지 확인 불가다.
신호 유실이 전혀 없다고 증명한 것도 아니지만, 관측된 추가 지연 대부분은 timeout 반환군이었다.

## 남긴 코드와 검증

- `WaitReturnStatistics.h`: 사유별 카운터·histogram·합산.
- `Worker::set_wait_return_diagnostics(bool)`: 작업 제출/시작 전 설정, 기본 OFF.
- `Worker::wait_return_statistics()`: Worker 시작 전 또는 join 이후에만 조회.
- 예제 환경 변수 `AREAUPDATE_WAIT_RETURN_DIAGNOSTICS=1`로 활성화.
- `WAIT_RETURN` 로그와 측정 폴더의 `wait-returns.csv`에 Worker별/전체 결과 기록.
- 기존 기본 Object 소스 복원 확인. 오브젝트 풀·idle Worker 관리·stealing 추가 없음.
- unit test: timeout·유한 신호·무기한 복귀, histogram/합산, live 조회 거부, 제출 후 설정 거부.
- 각 ON 실행에서 32개 Worker bucket/4개 전체 bucket, 기존 대기/기한 초과/notification 합계 검사 통과.
- 8회 모두 종료 코드 0, neverUpdated=0, Task 잔존 0.
- 최종 Release/Debug x64 솔루션 빌드·AreaTests, 양쪽 실행기의 self-test 통과.
- 전체 서식 검사 34개 파일과 명명·초기화·중괄호 검사 통과.

## 자료와 재현

- `artifacts/wait-return/{baseline,direct}`: 같은 계측을 포함한 실측 EXE/PDB.
- `artifacts/wait-return/measurements/results.csv`: ON/OFF 결과와 실행 파일 해시.
- 각 측정 폴더의 `wait-returns.csv`: timeout/신호/무기한/poll별 결과.
- `artifacts/wait-return/returns.csv`, `summary.csv`: ON 결과 집계.
- 원래 기본 Object 소스는 `artifacts/wait-return/source-current`에 보존.

새 경로로 재실행:

```powershell
.\Run-WaitReturnComparison.ps1 -OutputDirectory artifacts/wait-return/recheck
```
