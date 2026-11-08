# 짧은 기한 능동 대기 실험

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

## 결론

0.25·0.5·1·2ms 이하의 기한만 능동 대기로 바꾸는 후보를 구현해 20회 비교했다.
기본 Object 구현에서는 일관된 FPS 개선이 없었고, 전달 Task 제거 후보도 최대 약 1% 개선에 그쳤다.
2ms 후보는 CPU가 약 1.6%p 늘면서도 20FPS에 도달하지 못했다.
따라서 **실험 코드는 보관하고 기본 Core/예제 소스에는 반영하지 않았다.**
현재 기본 실행 파일에 `AREAUPDATE_ACTIVE_WAIT_US`를 지정해도 이 기능은 활성화되지 않는다.
재현 스크립트는 별도로 보관한 실험 EXE를 사용한다.

## 구현한 후보

- signal 방식이며, 알림을 지원하는 실제 유한 timer deadline이 있는 경우만 대상.
- 대기 진입 시 남은 시간이 경계 이하면 `Clock::now()`를 확인하며 대기한다.
- 새 요청·ticker 작업·깨우기·종료 요청이 보이면 바로 빠져나온다.
- 무기한 대기, poll 모드, 알림 없는 ticker의 주기 확인은 변경하지 않았다.
- 긴 대기를 일부러 일찍 끝내고 마지막 구간을 돌리는 방식은 아니다. 제안한 1번만 실험했다.
- 예약 시각·50ms 누락 정책·Actor 소유권·partition 공평성·round-robin을 유지했다.
- 신호 token은 종료 시 비차단 try-lock으로 소비하고, 잠금을 얻지 못하면 기존 대기 경로에 남겼다.
- 능동 대기 시간은 idle에 더하지 않고 별도 기록했다.
- 경계는 자체 루프의 진입 조건이다. OS 선점 때문에 실제 벽시계 복귀가 2ms 안에 보장되는 것은 아니다.

## 조건

- MSBuild Release x64, 논리 CPU 8개. 기준/후보 모두 같은 능동 대기·계측 코드를 포함해 새로 빌드했다.
- 기준은 기존 TaskExecuter, 후보는 보관한 전달 Task 제거 버전이다.
- timer Worker 8개 + Dispatcher 8개, Actor 70,000, 맵 320, 이동 시도 초당 2,000.
- seed 42/43, 각 20초, 1초 동안 1ms 간격 분산 입장, 추가 연산 0.
- 고정 20FPS 예약, 5ms × 256 타이머 휠, signal 대기, WPR OFF.
- Task·Worker 통계 ON, Actor 표본 256, Dispatcher 표본 OFF, 복귀 사유 계측 ON.
- 0/250/500/1000/2000µs × 두 구현 × 두 seed = 20회.
- 두 번째 seed는 경계·구현 실행 순서를 뒤집었다.
- 실행 중 빌드·정적 검사·무거운 분석은 병행하지 않았다.
- 아래는 두 실행의 정상 구간 FPS·CPU 산술 평균이다. CPU 100%는 전체 8개 논리 CPU다.
- 각 조건 두 번이므로 작은 개선의 통계적 유의성을 확정하지 않는다.

## FPS와 CPU

| 경계 | 기본 Object FPS / CPU | 전달 Task 제거 후보 FPS / CPU |
|---:|---:|---:|
| 0: 기존 방식 | 19.62 / 86.84% | 18.71 / 76.61% |
| 0.25ms | 19.51 / 87.12% | 18.86 / 77.22% |
| 0.5ms | 19.66 / 88.00% | 18.76 / 76.33% |
| 1ms | 19.40 / 86.64% | 18.69 / 75.30% |
| 2ms | 19.62 / 87.14% | 18.89 / 78.23% |

기본 Object의 0.5ms 평균 FPS는 약 0.2% 높았지만 회차별 방향이 엇갈렸다.
후보의 0.25ms/2ms는 두 회차 모두 소폭 상승했으나, 각각 약 0.8%/1.0% 수준이며 목표 갱신률 미달은 지속됐다.
CPU가 줄어든 일부 설정도 FPS가 함께 줄었으므로 CPU 절감만으로 우수하다고 판단하지 않았다.

## 타이머 지연과 최악 간격

후보 Object의 타이머 지연 P99 histogram 값 범위와 두 실행 중 최대 Actor 갱신 간격이다.
기록 길이가 짧아 최대 간격을 최악 지연의 보장값으로 해석하지 않는다.

| 경계 | 타이머 지연 P99 범위 | 최대 Actor 갱신 간격 |
|---:|---:|---:|
| 0 | 75~78ms | 151ms |
| 0.25ms | 70~77ms | 154ms |
| 0.5ms | 70~83ms | 198ms |
| 1ms | 78~82ms | 217ms |
| 2ms | 74~77ms | 269ms |

지연 개선 역시 일관되지 않았고, 2ms 설정에서도 50ms를 넘는 타이머 꼬리 지연은 남았다.

## 짧은 대기를 선택해도 효과가 작았던 이유

seed 42의 후보 Object에서 관측한 예다.

| 항목 | 기존 | 0.5ms | 2ms |
|---|---:|---:|---:|
| timeout 복귀 횟수 | 6,121 | 6,220 | 5,506 |
| timeout 기한 초과 누적 | 58,820ms | 57,475ms | 47,600ms |
| 남은 timeout의 예정 대기 평균 | 2.09ms | 2.52ms | 3.55ms |

짧은 기한을 능동 대기로 처리해도 경계보다 긴 실제 timeout은 기존 CV 경로에 남았다.
능동 대기 경로는 새 요청이 오면 조기에 종료하므로, 경로 진입 횟수를 제거한 timeout 횟수로 해석해서는 안 된다.
2ms 설정의 이 실행은 능동 대기 경로에 약 75만 회 진입했지만 누적 벽시계 시간은 약 4.35초였다.
실제 timeout을 충분히 줄이지 못하면서 처리/깨우기 패턴도 달라졌다는 것이 관측 결과다.
정확한 패턴 변화의 원인을 캐시·OS 스케줄러·특정 mutex에 단독 귀속하지 않는다.

기록한 activeWaitMs는 8개 Worker의 벽시계 합계로 OS 선점 시간도 섞일 수 있다.
실제 CPU 증가량은 별도 프로세스 CPU 측정으로 판단했다.

## 검증과 최종 상태

- 실험 구현: 유휴 상태 비회전, 가까운 deadline의 새 작업 처리, 조기 timer 실행 방지,
  신호 token 보존, 2ms 상한 검사, active/CV/전체 대기 수 일치 테스트 통과.
- 실험 기준/후보의 Release 빌드·AreaTests·self-test와 기본 구현 Debug 테스트 통과.
- 20회 모두 종료 코드 0, neverUpdated=0, Task 잔존 0.
- 각 실행에서 전체/active 복귀 수와 기존 기한 초과 카운터 일치 확인.
- 기본 소스로 복원했다. 따라서 확대 부하나 Area 모델의 능동 대기 성능을 추가 측정해 채택하지 않았다.
- 복원 후 Release/Debug x64 솔루션 빌드·AreaTests와 전체 서식·정적 규칙 검사를 다시 통과했다.
- Core/Object 기본 파일은 실험 전 스냅샷과 해시가 같고, 20회 상세 복귀 bucket 합계도 검증했다.
- 기본 코드에는 능동 대기 setter·루프·WakeSignal try_wait·active bucket이 남아 있지 않다.
- 측정 스크립트의 실험 로그 읽기 기능과 4/5 bucket 호환은 재현을 위해 유지했다.

## 보관 자료

- `artifacts/active-wait/{baseline,direct}`: 실험 EXE/PDB. 현재 기본 실행 파일과 다르다.
- `artifacts/active-wait/matrix/results.csv`: 20회 지표·실행 파일 해시.
- `artifacts/active-wait/summary.csv`: 집계.
- 각 실행의 runtime.log·summary.json·wait-returns.csv·CPU 표본.
- `source-current`: 복원한 기본 소스, `source-experiment`: 실험 Core/예제/테스트 소스.
  평면 파일 이름의 Worker/WakeSignal/WaitReturnStatistics는 Source/Core,
  main.cpp는 예제, RuntimeTests.cpp는 AreaTests에 해당한다.
- 후보의 별도 TaskExecuter 소스는 `direct` 폴더에 보존했다.

재현은 보관한 실험 바이너리를 사용하므로 현재 기본 소스를 다시 바꾸지 않는다.

```powershell
.\tools\experiments\Run-ActiveWaitComparison.ps1 -OutputDirectory artifacts/active-wait/recheck
```
