# Object 추가 최적화 4단계 비교

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

## 결론

제안한 네 단계를 순서대로 구현·검증·비교했다.
현재 부하 전반에서 일관된 처리량 개선을 보인 후보는 없었다.
기본 Task 통계와 기존 전달 Task/타이머 제출 경로는 유지한다.
최종 코드에는 Worker 진단 선택 기능과 회귀 테스트만 남겼고, 진단 기본값은 ON이다.
오브젝트 풀·idle Worker 관리·stealing·round-robin 정책 변경은 하지 않았다.

## 공통 조건

- MSBuild `Source/AreaUpdate.sln`, Release x64, 논리 CPU 8개.
- Object: Core timer Worker 8개 + TaskDispatcher Worker 8개.
- Actor 7만/28만, 맵 320/640, 이동 시도 초당 2,000/8,000회.
- 실행당 20초, seed 42/43, 1초 동안 1ms 간격 분산 입장.
- 추가 연산 0인 기존 Actor 예제, signal 대기, 고정 주기 20FPS, 타이머 휠 5ms × 256.
- 상세 프로파일 OFF. 실제 전투·AI 성능 시험이 아니다.
- 단계별 기준/후보 순서를 반대로 바꾼 두 쌍. 총 40회 실행했다.
- 아래 FPS·CPU는 정상 구간 두 실행의 산술 평균이다.
- 단계 사이 기준 수치 차이는 실행 변동을 포함하므로 각 행의 기준/후보만 비교한다.
- 실행 중 빌드·정적 검사를 병행하지 않았다. 두 번의 측정만으로 통계적 유의성을 확정하지 않는다.

## 1. Task 통계 OFF

기존 `TaskStatisticsEnabled=0` 빌드 옵션만 바꾸고 실행 코드는 별도로 최적화하지 않았다.

| Actor | FPS ON → OFF | CPU ON → OFF | FPS 변화 |
|---:|---:|---:|---:|
| 70,000 | 19.38 → 18.91 | 85.62 → 81.92% | -2.45% |
| 280,000 | 8.39 → 8.08 | 91.31 → 85.75% | -3.63% |

CPU는 줄었지만 FPS도 줄었다. 따라서 전역 atomic 통계 제거가 현재 signal 부하의 처리량 개선으로
곧바로 연결된다고 볼 수 없으며, 기본값은 ON으로 유지했다.
OFF 실행 4회에서는 Task 수명 검증을 미측정으로 표시했다. 0개 잔존을 확인했다고 해석하지 않는다.

## 2. 전달용 ExecuteTask 제거

Dispatcher 큐가 실행기의 `shared_ptr`를 직접 보관하는 후보를 구현했다.
일반 Task와 실행기 요청은 하나의 큐에서 처리하고, 소멸 후 완료 처리·실행기 수명·round-robin 배정을 유지했다.
이번 후보의 큐는 mutex로 보호한 vector를 Worker 소유 vector와 교환하는 방식이었다.
따라서 결과에는 전달 Task 제거뿐 아니라 큐 구현 변경의 효과도 함께 포함된다.

| Actor | 기준 FPS → 후보 FPS | CPU | FPS 변화 |
|---:|---:|---:|---:|
| 70,000 | 19.68 → 18.52 | 86.75 → 74.24% | -5.87% |
| 280,000 | 8.06 → 9.90 | 87.68 → 87.22% | +22.86% |

7만 seed 42의 실제 갱신당 Task 생성 수는 약 3.003 → 2.003으로 감소했다.
28만에서는 두 쌍 모두 개선됐지만, 7만에서는 두 쌍 모두 FPS가 낮아져 기본 경로로 채택하지 않았다.
7만 평균 최저 Actor FPS도 약 19.12 → 17.81로 낮아졌다.
이는 이 큐 구현 후보의 결과이며, 모든 종류의 전달 Task 제거가 나쁘다는 결론은 아니다.
후보 EXE/PDB와 `TaskExecuter.*`는 `artifacts/object-next/direct`에 보관했다.

## 3. Actor 타이머 예약 batch

2번을 제외한 원래 Dispatcher 구조에서 독립 구현했다.
Dispatcher Worker별 예약을 최대 64개 모아 목적 timer Worker별로 한 번에 공개했다.
각 Actor의 절대 만료 시각은 유지하고, Dispatcher batch 끝의 잔여 예약을 비운 후 마지막 완료를 처리했다.
Worker batch 등록은 FIFO 공개·pending 증감·깨우기를 묶고, 중단 후 거부 시에도 전달받은 Task를 정리했다.

| Actor | 기준 FPS → 후보 FPS | CPU | FPS 변화 |
|---:|---:|---:|---:|
| 70,000 | 19.48 → 18.77 | 86.57 → 81.20% | -3.63% |
| 280,000 | 8.24 → 8.14 | 88.74 → 87.27% | -1.17% |

28만에서는 한 쌍 상승·한 쌍 하락이었다. 최대 갱신 간격의 실행 간 최댓값도 605 → 679ms로 커졌다.
예약을 모으는 대기와 batching 효과의 순이득을 확인하지 못해 관련 지원 API·callback·실험 전용 테스트까지 제거했다.
후보 실행 파일은 `artifacts/object-next/timer-batch`에 보관했다.

## 4. 공통 Worker 진단 비용 분리

1~3번을 적용하지 않은 경로에서, 같은 바이너리의 ON/OFF만 비교했다.
Task 통계는 양쪽 모두 ON이고 Object와 Area에 동일한 Worker 옵션을 사용했다.

| 모델 / Actor | FPS ON → OFF | CPU ON → OFF | 최대 간격 ON → OFF |
|---|---:|---:|---:|
| Object / 70,000 | 19.59 → 19.38 | 86.71 → 84.02% | 167 → 269ms |
| Object / 280,000 | 8.27 → 8.60 | 89.55 → 89.50% | 529 → 567ms |
| Area / 70,000 | 20.01 → 20.00 | 29.23 → 30.16% | 124 → 109ms |
| Area / 280,000 | 19.25 → 19.33 | 87.81 → 86.21% | 126 → 190ms |

Object 28만은 약 3.98% 높은 FPS였지만, 모든 부하나 지연 지표에서 일관된 개선은 아니었다.
측정 비용을 구분하는 용도로 옵션은 유지하고 기본은 ON으로 남겼다.

### 최종 코드에 남긴 기능

- `Worker::set_runtime_diagnostics(bool)`: 작업 제출·시작 전에만 설정 가능.
- OFF는 요청 제출 시각, 큐 깊이 최댓값, 큐 대기/타이머 지연 측정용 시각 조회·갱신을 생략한다.
- pending·submitter·작업 수·busy/idle·Actor 갱신 간격·Task 수명 계측 등은 계속 유지한다.
  완전한 무계측 모드는 아니다.
- 상세 timer trace와 진단 OFF는 양립하지 않으므로 설정 시 예외로 거부한다.
- 예제는 `AREAUPDATE_WORKER_DIAGNOSTICS=0/1`로 선택하며 생략 시 ON이다.
- `tools/benchmark/Measure-RuntimeCpu.ps1`는 OFF의 `maxTimerDelayMs`, `maxQueueDelayMs`를 null로 기록한다.
  미측정 값을 0ms로 해석하지 않도록 ON/OFF와 표시 지원 여부도 기록한다.
- 이전 바이너리는 옵션 표시가 없고 항상 ON이었으므로 ON으로 해석하되 `workerDiagnosticsReported=false`로 구분한다.

초기 요약 파서에서 새 `RUNTIME_DIAGNOSTICS` 행을 누락한 문제를 수정했다.
해당 파서로 만든 일부 4번 요약은 원래 실행 로그의 정확한 표시로 보정했다.
보정 전 JSON도 `summary-before-metadata-fix.json`으로 보존했고 FPS·CPU 수치는 변경하지 않았다.

## 검증과 자료

- 후보마다 Release 솔루션 빌드, AreaTests, UpdateComparison self-test 통과.
- 2번: 외부 참조 해제 후 실행기 수명, Task 소멸 후 wait_idle 완료, 혼합 큐 FIFO,
  다중 생산자 직렬 실행, busy Worker를 건너뛰지 않는 round-robin 확인.
- 3번: 4생산자 합계 4,096개 예약으로 누락/조기 실행/잔존 확인.
  batch callback 완료까지 wait_idle이 반환하지 않는 검사도 통과했다.
- 최종 코드: 진단 OFF에서도 즉시/타이머 작업과 pending 정리가 유지되는지 확인.
  trace 충돌 설정 거부와 혼합 큐 FIFO 테스트를 유지했다.
- 마지막에 작업 제출 후 진단 설정 변경을 거부하는 경계를 보강했다. 이 설정 함수 변경 후 성능은 재측정하지 않았다.
- 경계 보강 후 최종 Release/Debug x64 솔루션 빌드와 AreaTests를 다시 통과했다.
- 전체 서식 검사 33개 파일, 명명·초기화·중괄호 검사, BOM/CRLF 및 `git diff --check` 통과.
- 성능 측정 40회 모두 실행 코드 0, neverUpdated=0. 통계 ON 36회 Task 잔존 0 확인.
- 결과: `artifacts/object-next/{stats,direct,timer,diagnostics}-comparison/results.csv`.
- 집계: `artifacts/object-next/summary.csv`. 기준 관련 소스는 `reference`, 단계별 EXE는 해당 후보 폴더.
- `tools/experiments/Run-ObjectNextComparison.ps1`로 보관한 두 EXE를 교차 비교할 수 있다.

진단만 재비교하는 예:

```powershell
.\tools\experiments\Run-ObjectNextComparison.ps1 -BaselineDirectory artifacts/object-next/diagnostics -CandidateDirectory artifacts/object-next/diagnostics -BaselineDiagnostics 1 -CandidateDiagnostics 0 -Modes job,area -OutputDirectory artifacts/object-next/diagnostics-repeat
```
