# 최신 3모델 고부하 비교: 42만·56만 Actor

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

## 결론

28만을 넘겨도 테스트할 수 있었다. 앞선 56만 중단은 실제 메모리 부족이 아니라 보수적인 사전 추정 때문이었다.
기존 실측 메모리에 여유분을 적용하고 실행 중 상한 검사를 추가해 42만·56만을 순차 실행했다.
세 모델 모두 실행·종료 검증을 통과했지만, 목표 20FPS에는 크게 미달했다.

## 조건

- 앞선 비교와 같은 보관 EXE/PDB 사용. 애플리케이션 소스·실행 정책은 변경하지 않았다.
- SHA256 `882C22FF194AF67CA7E4245D28B8B388AF0116265BC19D4080D682727A169356`.
- Release x64, 논리 프로세서 8개, Area/Area(old)는 Core Worker 8개.
- Object는 타이머 Worker 8개 + TaskDispatcher 8개. 스레드 총수까지 같은 비교는 아니다.
- 20초 × seed 42/43, 각 모델/인원 2회, 총 12회. 순서 순환 및 두 번째 seed 역순.
- 추가 연산 0인 같은 Actor 코드·일반 분산 배치·랜덤 이동. 실제 전투/AI 성능 시험이 아니다.
- 1,000ms 동안 1ms 간격 분산 입장, 고정 주기 20FPS, signal 대기, 타이머 휠 5ms × 256.
- 상세 timer/queue/CPU 추적 비활성화, Task 수명 통계 활성화.
- Area는 partition 모델, Area(old)는 최적화된 radius-lock 모델(`legacy`), Object는 Actor별 TaskExecuter(`job`).
- Actor 수에 비례해 맵과 이동 시도율을 확대했다.

| Actor | 맵 한 변 | 이동 시도/초 |
|---:|---:|---:|
| 420,000 | 784 | 12,000 |
| 560,000 | 906 | 16,000 |

## FPS와 CPU

정상 구간 FPS와 CPU의 두 실행 평균이다. CPU 100%는 논리 프로세서 8개 전체 용량이다.
28만 행은 이전 비교 결과를 참고로 표시한 것으로, 이번 12회에 포함된 재측정은 아니다.

| Actor | Area FPS / CPU | Area(old) FPS / CPU | Object FPS / CPU |
|---:|---:|---:|---:|
| 280,000 (이전) | 18.54 / 86.18% | 15.63 / 84.06% | 8.35 / 91.49% |
| 420,000 | 11.87 / 85.20% | 10.75 / 84.79% | 5.26 / 89.92% |
| 560,000 | 8.71 / 87.10% | 7.94 / 86.99% | 3.65 / 89.89% |

56만에서도 처리량 순위는 Area > Area(old) > Object였다.
다만 실행이 가능하다는 것과 게임의 목표 갱신률을 만족한다는 것은 다르다.
CPU가 100%가 아니라는 이유만으로 여유 용량이나 sleep 문제라고 단정할 수 없다.
이번 측정은 추가 원인 분석이나 최적화 실험을 포함하지 않는다.

## 갱신 편차와 지연

각 셀은 `최저 Actor FPS / P99 갱신 간격 상한 / 최대 갱신 간격`이다.
두 실행 중 나쁜 값을 사용했다. P99는 10ms histogram 구간 상한이다.

| Actor | Area | Area(old) | Object |
|---:|---:|---:|---:|
| 420,000 | 11.46 / 100ms / 135ms | 9.69 / 130ms / 239ms | 3.30 / 570ms / 712ms |
| 560,000 | 8.50 / 140ms / 244ms | 7.48 / 170ms / 323ms | 2.19 / 650ms / 817ms |

Area는 두 단계 모두 최저 FPS·P99·최대 간격도 더 좋았다.
20초 × 2회의 관측이며 장시간 최악 지연을 보장하는 결과는 아니다.

## 메모리와 안전 대책

이전 고정식은 56만의 필요량을 약 1,794MiB로 추정했다.
이번에는 같은 EXE의 기존 실측 private 메모리를 Actor 수·맵 면적 중 큰 증가 비율로 확대하고,
추가로 35%와 128MiB의 여유를 더했다. 사용 가능한 메모리의 70%라는 예산 자체는 유지했다.
42만은 추정 1,347MiB/예산 1,709MiB, 56만은 추정 1,756MiB/예산 1,766MiB로 사전 검사를 통과했다.
추정치는 실행 허용 판단을 위한 여유 포함 수치이며 실측 사용량은 아니다.

| Actor | Area 最大private | Area(old) 最大private | Object 最大private |
|---:|---:|---:|---:|
| 420,000 | 686.90MiB | 790.15MiB | 789.26MiB |
| 560,000 | 911.94MiB | 1,045.19MiB | 1,051.16MiB |

- `Measure-RuntimeCpu.ps1 -MaxPrivateMemoryMB`를 추가했다. 예산 초과 시 자식 프로세스를 종료하고 측정을 실패 처리한다.
- 약 250ms 간격 관측에 의한 보호이며 OS의 엄격한 메모리 할당 상한은 아니다.
- 본 측정에서 상한 초과는 없었다. OS 페이지가 전혀 교체되지 않았다고 증명하는 시험은 아니다.
- 측정 후 별도 프로세스에 의도적으로 1MiB 상한을 설정해 초과 시 자동 종료되는지 확인했다.
- 새 상한 인수를 생략한 기존 호출은 이전과 동일하다. 비교 스크립트는 예산을 명시한다.

## 검증과 재현

- 같은 보관 바이너리의 3모델 × uniform/boundary 이동·상호작용 검증 통과.
- 12회 모두 processExitCode=0, neverUpdated=0, tasksLive=0, Task 생성/파괴 검증 성공.
- 스크립트 구문 검사, CRLF 확인, `git diff --check` 통과. C++ 변경 없음.
- 원시 데이터: `artifacts/current-model-high-scale/results.csv`, 집계: `aggregate.csv`.
- 각 실행의 `summary.json`·`samples.csv`·`runtime.log` 보존.
- `environment.json`에 해시와 설정, `plan.csv`에 안전 예산 보존.

```powershell
.\Run-CurrentModelComparison.ps1 -ActorCounts 420000,560000 -SourceBinaryDirectory artifacts/current-model-comparison/bin -MemoryReferenceCsv artifacts/current-model-comparison/results.csv -OutputDirectory artifacts/current-model-high-scale-repeat
```
