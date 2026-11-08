# Area 원본 / Area 수정본 / Actor Job 결과

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

## 최종 적용 구성

- Git 원본: `b713018`의 반경 lock, random queue, head 충돌 중단, 전체 Area 실행 유지.
- Area 수정본: 반경 2의 공간 안전 간격 5와 batch 64 유지. Active 필터, pending 변경 없는 경우 잠금 생략, 순서 보존 목록 갱신, deque queue 적용.
- Actor Job: 오브젝트 풀 없음. 기존 round-robin 재전달과 TLS 정책 유지. 마지막 dispatcher 작업 완료 때만 완료 통지 mutex를 획득하도록 변경.
- 공용/스레드별 풀, idle stealing, TLS 대기 확대는 최종 적용에서 제외. 세부 사항은 `OPTIMIZATION.md` 참고.

## 측정 조건과 범위

Windows 논리 프로세서 8개, MSBuild 솔루션 x64 Release. 각 시나리오 100 tick, 별도 World 준비 실행 5 tick, seed 42, 방식별 3회, 실행 순서 회전. 총 7개 대표 시나리오 × 3방식 × 3회 = 63회다. 이전 13개 조건을 모두 3방식으로 재실행한 것은 아니다.

Git 원본은 원본 main/Worker 전체가 아니라, 원본 Area/AreaExecuter를 공통 시험기에 연결한 것이다. 수정 Area도 공통 시험기의 partition barrier 실행을 사용한다. 원본은 매 tick 전체 맵을 enqueue하며 수정본은 Active Area만 처리하므로, 결과에는 공간 스케줄링뿐 아니라 inactive skip 효과도 포함된다. 모든 방식의 Actor 위치는 고정이고 상호작용은 가산 damage다. 실제 접속자 처리량으로 해석하지 않는다.

측정 바이너리 SHA256: `4E019B9027466E8E9595979CD083A50DC36FC96F5F09E13566115979FDD0FFA7`.

부하 편중 재현 self-test는 측정 후 추가했다. 측정 당시 바이너리는 `artifacts/scenarios-three-way/UpdateComparison-measured.exe`로 보관했다. 이후 self-test 추가 빌드는 이 바이너리를 덮어쓰지 않는다.

## 결과

단위는 초, 값은 3회 중앙값이다. 모든 조건에서 Worker는 8개다.

| ID | 조건 | Area 원본 | Area 수정본 | Actor Job |
|---|---|---:|---:|---:|
| S04 | 5,000 Actor 중앙 집중, 상호작용 8, 연산 1,000 | 2.757 | 0.662 | 1.537 |
| S05 | 5,000 Actor 중앙 집중, 상호작용 32, 연산 1,000 | 2.879 | 0.716 | 3.399 |
| S06 | 2,000 Actor 한 Area, 상호작용 32, 연산 1,000 | 3.178 | 0.642 | 1.651 |
| S10 | 5,000 Actor 중앙 집중, 상호작용 8, 연산 20,000 | 6.328 | 9.516 | 3.434 |
| S11 | 2,000 Actor 한 Area, 상호작용 8, 연산 20,000 | 9.820 | 6.667 | 1.333 |
| S14 | 640×640 맵, 20,000 Actor 분산, 연산 1,000 | 10.351 | 1.092 | 7.586 |
| S15 | 640×640 맵, 20,000 Actor 분산, 연산 20,000 | 22.282 | 13.431 | 11.575 |

S04~S11의 전체 맵은 320×320이며 중앙 집중은 64×64 배치다. S14/S15의 상호작용 횟수는 8이다.

## 해석

- S05는 수정 Area가 원본보다 4.02배, Job보다 4.75배 빨랐다. 원본의 빈 Area 실행 제거 효과도 포함된다.
- S10은 원본 Area가 수정본보다 1.50배 빨랐다. 수정본의 색상별 barrier와 batch 64에 따른 제한이 있으며, 수정 구조가 모든 조건에서 원본보다 빠르지는 않다.
- S11은 Job이 수정 Area보다 5.00배 빨랐다. 한 Area 내부의 Actor 업데이트는 여전히 직렬이다.
- 큰 맵 S14에서는 수정 Area가 Job보다 6.95배 빨랐다. 무거운 연산의 S15에서는 Job이 1.16배 빨랐다. 중앙 집중 S10보다 두 방식의 차이가 작아졌지만, Actor 수·밀도도 다르므로 맵 크기 하나만의 인과 효과라고 단정하지 않는다.
- 같은 5,000 Actor 중앙 집중에서는 색상당 공간 작업 수가 제한된다. 빈 땅만 늘리기보다 Active Area가 넓게 분포해야 현재 batch를 유지하면서 병렬성을 확보할 수 있다.
- 기존 `TEST-RESULTS.md`와는 측정 시점·순서가 다르므로, 그 시간과 이번 시간을 단순 비교해 각 미세 최적화의 효과를 확정하지 않는다. 이번 표는 Git 원본 구조와 현재 구조의 비교다.
- 연산 1,000/20,000은 동일 shift/XOR 반복량이며 실제 게임 로직의 가벼움/무거움을 보증하지 않는다.

## 정확성 및 통합시험

- 최종 63회 모두 Actor별 업데이트 수·수신 damage 검증 통과. 대응하는 세 방식의 checksum과 생성 이벤트 수 일치. 동시 접근 위반 합계 0.
- Release/Debug 솔루션 빌드 및 AreaTests 통과. 반경 1/2 공간 분리와 512 Actor 대량 이동·제거 시험 포함.
- `UpdateComparison --test`에 4 Worker 중 3개가 일을 마쳤는데도 바쁜 Worker 뒤의 독립 작업이 대기하는 재현 시험 추가. 이 시험은 dispatcher 경로의 한계를 확인하며 JobExecuter 재전달의 효과를 부정하는 시험은 아니다.
- 실제 실행기 30.267초 시험: Actor 10,000개, 이동 48,889회, 업데이트 5,663,183회, 평균 18.711 FPS. 중복 업데이트 0, 미실행 Actor 0, pending/live Task 0. 최대 업데이트 간격 111ms, p99 구간 50ms.

## 자료와 재현

최종 결과는 `artifacts/scenarios-three-way/runs.csv`, `summary.csv`, `environment.json`에 있다. 실제 실행기 로그는 `artifacts/runtime-optimized.log`다. 메모리는 모델별 전체 맵 자료구조가 달라 동등한 서버 AOI 메모리 비교가 아니다. 각 조건 3회뿐이며 시스템 부하와 실행 변동을 통제한 통계적 결론은 아니다.

```powershell
.\Run-ScenarioComparison.ps1 -WithOriginal -Only 'S04,S05,S06,S10,S11,S14,S15'
# 같은 바이너리·설정의 중단된 실행 이어서 수행
.\Run-ScenarioComparison.ps1 -WithOriginal -Only 'S04,S05,S06,S10,S11,S14,S15' -Resume
```
