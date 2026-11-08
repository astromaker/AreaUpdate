# 최신 Area / Area(old) / Object 성능 비교

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

## 결론

현재 합성 Actor·랜덤 이동 부하에서는 Area가 가장 낮은 CPU 비용으로 높은 갱신률을 유지했다.
최적화한 Area(old)도 14만 Actor까지 약 20FPS를 유지했고, Object는 7만부터 목표에 미달했다.
28만에서는 Area도 약 18.54FPS로 목표 20FPS에 미달한다.
이는 실제 전투·AI 로직을 넣은 게임 서버의 수용 인원이나 모든 작업 부하의 우열을 의미하지 않는다.

## 비교 대상

| 표기 | 실행 모드 | 현재 구현 | 스레드 구성 |
|---|---|---|---|
| Area | area | 개선된 partition scheduling, active Area, 고정 주기, 입장 제한 unlimited | Core Worker 8개 |
| Area(old) | legacy | 이번에 최적화한 radius lock, active Area, batch 64, 정렬된 타이머 제출 | Core Worker 8개 |
| Object | job | Actor별 TaskExecuter, TaskDispatcher, 고정 주기 | 타이머 Worker 8개 + Dispatcher 8개 |

Area(old)는 동결된 초기 원본(`original`)이 아니다. 최신 최적화 버전이다.
Object는 현재 제품 코드의 구성을 그대로 측정했으며 총 실행 스레드 수까지 동일한 알고리즘 비교는 아니다.
Area는 공간 보호 아래 실행하고 Object는 Actor별 직렬 실행과 메시지 전달을 사용하므로 동기화 계약도 다르다.
애플리케이션 코드는 이번 비교를 위해 변경하지 않았다.

## 조건

- 2026-11-08, `Source/AreaUpdate.sln` MSBuild Release x64, 논리 프로세서 8개.
- 동일한 보관 실행 파일: SHA256 `882C22FF194AF67CA7E4245D28B8B388AF0116265BC19D4080D682727A169356`.
- 목표 20FPS, 고정 주기 예약, signal 대기, 타이머 휠 5ms × 256.
- 한 실행 20초, 첫 1,000ms 동안 1ms 간격으로 분산 입장.
- 각 Actor 수와 모델에 seed 42/43 각 1회, 총 24회 완료.
- 실행 순서를 인원 단계마다 순환하고 두 번째 seed에서 역순으로 실행.
- 같은 TestObject 코드, Actor 추가 연산 0. timestamp·중복 실행 검사·갱신 집계 비용은 포함.
- 일반 분산 배치, 동일한 입력 이동 시도율. 시간 기반 이동이라 실제 성공 이동 수는 모델마다 다르다.
- 맵 한 변 `ceil(320 × sqrt(Actor 수 / 70000))`, Area 한 칸 50, radius 2 대각선 범위 유지.
- 이동 시도율 `ceil(Actor 수 × 2000 / 70000)`회/초.
- Task 수명 통계 활성화. 선택적 CPU·queue·timer 상세 추적은 비활성화.
- 측정은 순차 실행했고 다른 빌드·무거운 검사를 병행하지 않았다.

| Actor | 맵 한 변 | 이동 시도/초 |
|---:|---:|---:|
| 10,000 | 121 | 286 |
| 70,000 | 320 | 2,000 |
| 140,000 | 453 | 4,000 |
| 280,000 | 640 | 8,000 |

## FPS와 CPU

아래는 정상 구간 FPS·CPU의 두 실행 산술 평균이다. CPU 100%는 논리 프로세서 8개 전체 용량이다.
FPS는 입장 후 정상 구간 Actor별 갱신 횟수로 계산한 `steadyActorFps`를 사용했다.
CPU는 측정 도구가 선택한 입장 완료 이후 표본 구간이며 FPS와 구간 경계가 완전히 같지는 않다.

| Actor | Area FPS | Area CPU | Area(old) FPS | Area(old) CPU | Object FPS | Object CPU |
|---:|---:|---:|---:|---:|---:|---:|
| 10,000 | 20.00 | 4.43% | 20.00 | 7.76% | 20.00 | 28.80% |
| 70,000 | 20.01 | 28.39% | 20.01 | 33.28% | 19.56 | 86.87% |
| 140,000 | 19.98 | 56.11% | 19.99 | 59.28% | 17.58 | 90.90% |
| 280,000 | 18.54 | 86.18% | 15.63 | 84.06% | 8.35 | 91.49% |

20FPS 부근의 작은 초과·미달은 관측 구간 및 ms 해상도의 영향을 포함한다. 정확히 모든 간격이 50ms라는 뜻은 아니다.
28만에서 Area는 Area(old)보다 약 18.6%, Object보다 약 121.9% 높은 평균 FPS였다.
Area(old)의 CPU가 Area보다 약간 낮아도 처리량이 낮으므로 효율이 더 높다는 뜻은 아니다.

## 갱신 편차와 순간 지연

각 셀은 `최저 Actor FPS / P99 갱신 간격 상한 / 최대 갱신 간격`이다.
평균 FPS가 아니라 두 실행 중 가장 나쁜 값을 사용했다.
P99는 10ms 폭 histogram의 구간 상한이며 정밀 percentile 값이 아니다.

| Actor | Area | Area(old) | Object |
|---:|---:|---:|---:|
| 10,000 | 19.93 / 60ms / 100ms | 19.91 / 60ms / 100ms | 19.88 / 60ms / 113ms |
| 70,000 | 19.89 / 70ms / 116ms | 19.90 / 60ms / 105ms | 18.76 / 80ms / 148ms |
| 140,000 | 19.79 / 70ms / 117ms | 19.85 / 60ms / 146ms | 15.76 / 110ms / 225ms |
| 280,000 | 17.92 / 70ms / 174ms | 14.53 / 90ms / 206ms | 6.41 / 340ms / 448ms |

Area가 모든 부하의 모든 지연 지표에서 우세한 것은 아니다. 7만의 P99·최대 간격은 Area(old)가 작았다.
28만에서는 Area가 평균·최저 FPS와 P99·최대 간격 모두 더 좋았다.
20초 × 2회 시험이므로 장시간 최악 지연 보장이나 엄밀한 최대 수용 인원 판정에는 사용할 수 없다.

## 검증과 미측정 범위

- Release 빌드·AreaTests 통과.
- 3모델 × uniform/boundary에서 각 3초·5,000 Actor 이동·상호작용 검증 통과.
  모두 overlaps=0, sent=applied, tasks_live=0.
- 본 측정 24회 모두 processExitCode=0, neverUpdated=0, tasksLive=0, Task 생성/파괴 검증 성공.
- 최초 56만 Actor 단계는 보수적인 사전 메모리 검사에서 중단했다.
  추정 1,793.83MiB에 비해 당시 허용 예산은 1,766.19MiB였다.
  실제 메모리 부족이나 실행 불가의 증명이 아니라 안전 여유를 포함한 사전 판단이었다.
  후속 실측 기반 안전 검사에서는 42만·56만 모두 실행을 마쳤다. `CURRENT-MODEL-HIGH-SCALE.md` 참고.
- 본 표는 기본 Actor 갱신·이동 비교이며 검증용 상호작용 시험의 속도는 혼합하지 않았다.
- 실제 게임의 전투·AI·시야 처리는 자료가 없어 확인 불가.
- 이전 결과는 예약·대기·실행 코드·계측 조건이 달라 이 표와 혼합하지 않는다.

## 재현 자료

- `Run-CurrentModelComparison.ps1`: 최신 Release를 보관하고 3모델을 같은 조건으로 측정.
- `artifacts/current-model-comparison/bin`: 실측 EXE/PDB.
- `environment.json`: 실행 조건·해시, `plan.csv`: 맵·이동 수·메모리 예산.
- `results.csv`: 전체 24회 측정값, `aggregate.csv`: 집계·실행 간 FPS 범위.
- 각 실행 폴더: `runtime.log`, `stderr.log`, `samples.csv`, `summary.json`.

재측정할 때는 기존 결과를 덮어쓰지 않도록 새로운 출력 경로를 지정한다.

```powershell
.\Run-CurrentModelComparison.ps1 -OutputDirectory artifacts/current-model-comparison-repeat
```
