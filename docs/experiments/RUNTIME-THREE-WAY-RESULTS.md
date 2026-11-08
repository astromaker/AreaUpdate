# 분산 입장 런타임 3종 비교

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

이 문서는 세 방식이 모두 완료 기준 +50ms를 사용하던 측정 결과다.
후속 Object 고정 주기 변경과 1만 개 재측정은 FIXED-PERIOD-RESULTS.md를 참고한다.

## 범위

개선 Area의 scheduler·sleep·partition 정책은 수정하지 않았다.
실행 예제에 area/original/job 모드를 추가해 같은 Actor 코드로 다시 측정했다.
기존 UpdateComparison의 고정 tick·일괄 입장 시험과 다른 결과다.

| 모드 | 실행 경로 | 처리 스레드 구성 |
|---|---|---|
| area | 현재 AreaExecuter, active Area, partition scheduling | Core Worker 8개 |
| original | 원본 Area/AreaExecuter의 radius lock, 랜덤 큐, head blocking, 전체 Area 실행 | 공통 Core Worker 8개 |
| job | Actor별 TaskExecuter, TaskDispatcher, Actor별 재예약 | 타이머용 Core Worker 8개 + TaskDispatcher 8개 |

총 논리 CPU는 8개이며 CPU 사용률은 프로세스 전체를 합산했다.
job은 실제 이 결합 구성에서 실행 스레드가 16개다. 총 스레드 예산까지 같은 순수 알고리즘 비교는 아니다.
ObjectUpdate의 기존 TaskDispatcher와 Core 타이머를 수정하지 않고 연결한 비용을 포함한다.
Worker busy/idle은 Core Worker만 집계하므로 job 실행 스레드 전체의 busy/idle로 해석하면 안 된다.

## 공통 조건

- Release x64, seed 42, Worker timer wheel 5ms 단위, 유휴 sleep_for(1ms).
- Actor당 추가 연산 0. 갱신 간격·중복 실행·Task 수명 계측은 같은 TestObject 코드다.
- 각 실행은 맵 준비 후 20초, 첫 5,000ms에 분산 입장, 입장 확인 요청 간격 1ms.
- 한 변의 Area 수는 ceil(320 × sqrt(Actor 수 / 70000))로 밀도를 유지한다.
- Area 한 칸 50, radius 2의 대각선까지 포함한다.
- 이동 시도는 ceil(Actor 수 × 2000 / 70000)회/초다.
- 현재 예제와 동일하게 완료 시점 + 50ms로 재예약한다. 절대 시각 20Hz로 변경하지 않았다.
- 인원별 모드 실행 순서를 순환했고 최대 인원은 job→original→area 순서로 1회 반복했다.

CPU는 실제 입장 완료 이후 약 1초부터의 표본 평균이다.
FPS는 입장 후 Actor 시간 합계를 분모로 한 전체 실행 평균이므로 CPU와 구간이 정확히 같지는 않다.
시계 기반 입장·이동이므로 같은 seed라도 완전히 동일한 이벤트 타임라인을 재생하는 시험은 아니다.
입력 이동 시도율은 같지만 실제 이동 성공 수는 구현별 pending 처리 지연에 따라 다르다.

## FPS

각 기본 단계는 1회 탐색 결과다.

| Actor | 맵 한 변 | 개선 Area | 예전 Area | ObjectUpdate |
|---:|---:|---:|---:|---:|
| 10,000 | 121 | 15.862 | 15.377 | 15.589 |
| 30,000 | 210 | 15.360 | 15.151 | 15.579 |
| 70,000 | 320 | 15.118 | 12.651 | 14.154 |
| 140,000 | 453 | 11.403 | 6.047 | 12.648 |
| 280,000 | 640 | 5.684 | 2.781 | 7.287 |
| 560,000 | 906 | 2.688 | 1.232 | 3.399 |

## CPU

전체 논리 CPU를 합쳐 100%로 환산했다.

| Actor | 개선 Area | 예전 Area | ObjectUpdate |
|---:|---:|---:|---:|
| 10,000 | 2.21% | 8.67% | 29.71% |
| 30,000 | 5.73% | 24.10% | 71.44% |
| 70,000 | 11.60% | 49.24% | 82.42% |
| 140,000 | 16.01% | 50.16% | 91.54% |
| 280,000 | 16.11% | 51.01% | 88.12% |
| 560,000 | 16.28% | 49.08% | 90.72% |

## 최대 인원 반복

| 모드 | 최초 FPS / CPU | 반복 FPS / CPU |
|---|---:|---:|
| 개선 Area | 2.688 / 16.28% | 2.733 / 16.21% |
| 예전 Area | 1.232 / 49.08% | 1.260 / 48.37% |
| ObjectUpdate | 3.399 / 90.72% | 3.394 / 88.87% |

56만 최초 실행의 p99 갱신 간격 버킷은 개선 Area 440~449ms,
ObjectUpdate 720~729ms, 예전 Area 1,000ms 이상이었다.
예전 Area의 마지막 버킷은 상한으로 묶인 값이며 정확히 1,000ms를 뜻하지 않는다.
ObjectUpdate가 평균 FPS는 높지만 이 단계의 p99도 가장 좋았던 것은 아니다.

같은 실행의 Private Bytes 표본 최대는 개선 Area 약 883MiB,
예전 Area 약 1,133MiB, ObjectUpdate 약 1,089MiB였다.
어댑터와 공통 계측·공통 맵을 포함하므로 각 엔진의 최소 메모리 요구량 비교는 아니다.

## 해석

작은 인원에서 세 방식 모두 약 15~16FPS였고, 낮은 부하에서도 20FPS 목표를 달성하지 못했다.
공통 sleep·타이머와 완료 기준 예약의 영향도 함께 존재하는 시험이다.

큰 인원에서 개선 Area의 낮은 CPU·처리량 정체가 다시 나타났다.
ObjectUpdate는 CPU를 더 사용하며 이 구성의 큰 인원 평균 처리량이 가장 높았다.
예전 Area는 더 많은 CPU를 쓰면서도 개선 Area보다 평균 FPS가 낮았다.

이 결과로 Actor 방식 자체가 항상 우월하거나, 특정 인원이 실제 게임의 수용 한계라고
결론내릴 수는 없다. Actor가 수행하는 것은 가벼운 공통 계측이며 이웃 간 피해·시야·전투 처리는 없다.
특히 job은 Actor별 timer Task → TaskExecuter 작업 → dispatcher 실행 요청을 거치는 비용을 포함한다.
기존 고정 tick 벤치마크에는 없던 비동기 예약 계층이 있으므로 두 결과를 섞어서는 안 된다.

## 어댑터의 한계

original은 b713018의 Area/AreaExecuter 소스를 수정 없이 사용한다.
Actor 등록·탈퇴·이동은 공통 Area payload를 원본 Area 안에서 호출해 재사용했다.
원본 주변 lock이 payload 실행을 보호하고 빈 Area도 계속 예약한다.
시작 때 전체 맵을 큐에 등록하고 이후 공통 Worker에서 50ms 뒤 재예약한다.
원본 main의 초기 0~50ms 랜덤 지연이나 원본 Worker 구현 전체를 그대로 실행한 것은 아니다.

job은 Actor별 TaskExecuter가 update를 직렬 실행한다.
이동은 같은 좌표 범위에서 위치와 공통 공간 점유 표시를 갱신하지만 Area 소속 리스트를 사용하지 않는다.
이 시험의 update는 위치나 이웃 상태를 읽지 않으므로 동시 위치 변경과 Actor 계산 사이의 공유 읽기는 없다.
Area 모드의 실제 pending 등록·탈퇴 비용과 job의 좌표 변경 비용이 같다는 뜻은 아니다.

공통 맵은 모든 모드에 할당한다. job의 active Area 표시는 점유 통계이며 update 대상 큐가 아니다.
original의 sleep blocked 지표는 마지막 원본 run의 실행 가능 결과를 바탕으로 한 어댑터 값이다.
개선 Area의 partition admission 대기 비율과 직접 비교하지 않는다.

## 검증과 보존

- 6단계 × 3모드 + 최대 인원 3모드 반복 = 총 21회.
- 한 측정 실행 파일과 같은 Actor당 연산량을 사용했다.
- 모든 런타임 실행은 종료 코드 0, 중복 실행·미갱신·잔여 Task 0이었다.
- Release/Debug 빌드와 각 모드의 작은 입장·이동·종료 smoke 테스트를 통과했다.
- 스타일 검사와 변경 금지 원본 스냅샷 검증을 수행했다.
- 별도의 공통 AreaTests에서 간헐적인 0xC0000409 종료가 관찰됐다.
  재시도 통과와 별개로 원인은 미확인이다. 신규 모드의 런타임 검증과 구분해 기록한다.

자료는 artifacts/runtime-three-way의 results.csv, plan.csv, environment.json,
binary.json, 각 모드·인원별 runtime.log/summary.json/samples.csv에 있다.
측정 실행 파일도 보존했다.

```powershell
# 마지막 인수: area / original / job
.\artifacts\runtime-three-way\AreaUpdateExample-measured.exe 20 70000 2000 42 5000 1 0 320 original
.\artifacts\runtime-three-way\AreaUpdateExample-measured.exe 20 70000 2000 42 5000 1 0 320 job
.\tools\benchmark\Measure-RuntimeCpu.ps1 -Mode job -Actors 70000 -MapSide 320 -ExecutablePath artifacts/runtime-three-way/AreaUpdateExample-measured.exe -OutputDirectory artifacts/new-job-run
```

tools/benchmark/Run-RuntimeComparison.ps1이 공통 메모리 안전 예산 안에서 비교한다.
기존 자료를 덮어쓰지 않도록 보호하며, 기존 측정 결과와 다른 실행 파일을 혼합하지 않는다.
