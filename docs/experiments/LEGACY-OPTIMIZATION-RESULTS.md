# 예전 Area 최적화와 새 Area 비교

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

후속 잠금 계측·예약 묶음·batch 검증은 [RADIUS-FOLLOWUP-RESULTS.md](RADIUS-FOLLOWUP-RESULTS.md), 최신 묶음 게시 개선과 기본값 변경은 [RADIUS-PUBLICATION-RESULTS.md](RADIUS-PUBLICATION-RESULTS.md)에 정리했다.

## 구성과 보존 범위

- `area`: 완료된 새 Area. 무제한 파티션 scheduling, 활성 예약, 고정 50ms 주기, 단일 할당 예약 묶음을 그대로 사용한다.
- `original`: Git 스냅샷의 예전 Area와 기존 어댑터. 원본을 수정하지 않았다.
- `legacy`: 새로 추가한 예전 방식 최적화 경로. 공간 파티션을 도입하지 않고 radius footprint 잠금을 유지한다.

새 코드 위치는 `Source/AreaUpdateLegacy/Optimized/RadiusExecuter.*`다. 예제의 RuntimeBackend와 이동·이벤트 검증에 legacy 모드를 연결했다. 새 Area 구현과 원본 스냅샷은 변경 전 파일 해시를 보관해 변경 여부를 확인한다.

## 최적화 내용

### 실행과 예약

원본 경로는 모든 Area를 초기 큐에 넣고 완료 후 50ms 상대 예약을 반복했다. 최적화 경로는 기존 payload Area의 예약 소유권을 사용해 active Area만 등록한다. 등록·이동·이벤트 등의 기존 wake 경로로 재활성화하며 비활성화되면 재예약하지 않는다. 고정 주기 예약은 지난 절대 시각을 기준으로 다음 tick을 계산하고, 이미 지나간 tick은 건너뛴다.

Worker와 공통 타이머 휠은 바꾸지 않았다. 반경 방식의 재예약은 아직 Area별 Task를 사용한다. 새 Area의 예약 묶음을 그대로 가져오거나 원본 Worker를 새로 만든 것은 아니다.

### 큐와 알림

원본의 rand 분산과 list 큐 대신, 고정된 좌표 기반 bucket과 deque를 사용한다. Worker는 예전 방식처럼 큐를 순환 탐색하며 한 번에 최대 64개만 추출한다. **첫 잠금 획득 실패 시 해당 큐 추출 중단 정책은 유지**했다. 뒤의 Area를 건너뛰는 정책은 추가하지 않았다.

작업 등록·잠금 해제에 따른 generation과 기존 Worker의 signal을 연결한다. 실패한 탐색은 새 작업이나 잠금 해제 변화가 있어야 재시도할 수 있다. 활성 Runner가 없을 때는 게시 완료 후 알림을 보장해, 생산자 큐 mutex 경합 직후의 lost wake를 방지한다. 별도 idle Worker 관리나 stealing 시스템은 추가하지 않았다.

### 반경 잠금

1차 구현은 셀별 bool 잠금을 정렬된 순서로 획득하고 acquire/release 메모리 순서를 사용했다. 실패 시 이미 획득한 잠금을 해제한다. 원본의 seq_cst 잠금과 해제, 전역 진단 카운터, 큐/예약 비용이 함께 달라졌으므로 개선을 단일 원인의 효과로 해석하지 않는다.

2차 구현은 같은 셀 집합을 비트마스크로 표현했다. 큰 맵의 내부 radius 2는 5×5=25셀을 보호하며, 보통 행당 1~2개 워드로 묶인다. 워드 전체를 독점하는 것이 아니라 원하는 bit만 CAS로 설정한다. **같은 64-bit 워드 안에서도 bit가 겹치지 않는 두 footprint는 동시 실행할 수 있다.** 공간 파티션이나 64셀 묶음 배타 실행이 아니다.

획득 실패 시 해당 시도의 bit만 해제하고, 해제는 fetch_and로 다른 실행자의 bit를 보존한다. 현재 Actor update가 끝난 뒤 잠금을 해제한다. 잠금 footprint는 실행 전 고정하며 lock table은 모든 Worker와 Area보다 오래 살아 있어야 한다.

추가 후보에서는 워드 간격을 64바이트로 늘리고 footprint mask vector 용량을 정확히 예약했다. 추가 성능 이득이 확인되지 않아 이 두 변경은 제외하고, 측정한 2차 밀집 bitmap 구현을 최종안으로 유지했다. 공유 잠금표는 오브젝트 풀이 아니다.

## 안전성 검증

- radius 2의 대각선·거리 4 footprint 충돌과 거리 5의 독립 실행 검사.
- 같은 bitmap 워드 안의 서로 다른 bit 동시 획득, 한쪽 해제가 다른 소유자의 bit를 지우지 않는지 검사.
- 앞 워드를 획득한 뒤 뒤 워드에서 실패했을 때 rollback되는지 검사.
- Area callback 예외 시 해당 Area의 footprint 해제 검사. Worker callback 예외를 복구하는 실행 모델은 추가하지 않았다.
- 4 Worker에서 121개 Area를 각각 20회 재제출하고 보호 셀의 동시 진입이 없는지 검사.
- 실제 payload의 이동·외부 이벤트·탈퇴·재등록·비활성화 검증. Actor inbox와 소유권 구현은 공통 payload를 유지한다.

서식 도구가 예전 폴더 전체를 제외하던 설정은 Baseline과 OriginalAreaAdapter만 제외하도록 좁혔다. 새 Optimized 코드도 일반 서식·명명·초기화·중괄호 검사 대상이다. 원본 파일은 재포맷하지 않는다.

## 성능 실험

MSBuild Release x64, Task 통계 OFF, Area 전역 카운터 OFF, Worker 8개, signal 대기, radius 2, Actor 추가 연산 0. 1,000ms 동안 1ms 간격 분산 입장이다. Actor 수에 맞춰 맵도 키워 Area 밀도를 비슷하게 유지했다.

| 사례 | Actor | 맵 | 이동 요청/초 |
|---|---:|---|---:|
| 균등 7만 | 70,000 | 320×320 | 2,000 |
| 균등 14만 | 140,000 | 453×453 | 4,000 |
| 균등 28만 | 280,000 | 640×640 | 8,000 |
| 한곳 집중 7만 | 70,000 | 320×320 | 2,000 |

각 실행 30초, seed 42는 새 Area→예전 최적화, seed 43은 역순으로 비교한다. 평균 FPS와 CPU는 정상 상태 구간을 사용하며 CPU는 전체 논리 프로세서 대비다. 시점이 다른 최적화 단계 간 결과는 엄밀한 단일 변수 A/B 실험이 아니다. 실제 게임 로직 성능도 아니다.

### 1차: 셀별 bool CAS

| 사례 | 새 Area FPS | 예전 최적화 FPS | 새 CPU | 예전 CPU |
|---|---:|---:|---:|---:|
| 균등 7만 | 19.993 | 19.971 | 23.08% | 45.38% |
| 균등 14만 | 19.995 | 16.234 | 44.18% | 79.45% |
| 균등 28만 | 19.255 | 8.217 | 83.20% | 85.01% |

활성화와 예약을 개선해도 셀별 잠금 방식의 비용이 컸다. 이 버전의 소스와 실행 파일은 참고용으로 보관하고 bitmap 후보로 대체했다.

### 2차: 밀집 bitmap

14만 예전 최적화는 seed 42에서 19.922 FPS/CPU 67.19%, seed 43에서 19.993 FPS/68.59%였다. 28만은 11.658/11.757 FPS로 1차보다 높아졌지만 새 Area보다 낮았다. 이후 워드 간격·mask 저장 공간을 보완한 후보를 추가 비교했다.

잠금 거절 카운터는 '큐 앞 Area의 footprint 획득 실패' 횟수다. 하드웨어 CAS 재시도 전체나 원본의 neighbor-only FAIL_COUNT와 같은 지표가 아니다.

## 최종 비교

2차 밀집 bitmap, 각 사례 seed 42/43의 정상 상태 평균이다.

| 사례 | 새 Area FPS | 예전 최적화 FPS | 새 CPU | 예전 CPU |
|---|---:|---:|---:|---:|
| 균등 7만 | 19.987 | 19.987 | 25.13% | 38.85% |
| 균등 14만 | 19.988 | 19.957 | 44.04% | 67.89% |
| 균등 28만 | 19.653 | 11.708 | 83.94% | 86.14% |
| 한곳 집중 7만 | 19.973 | 19.966 | 2.84% | 4.47% |

1차 셀별 CAS 후보 대비 28만 FPS는 8.217에서 11.708로 약 42.5% 높아졌다. 잠금 표현뿐 아니라 알림과 카운터 처리도 보완했으므로 bitmap 하나만의 효과로 해석하지 않는다. 수정하지 않은 original 대비 향상률을 측정한 표는 아니다.

64바이트 간격 후보의 예전 최적화 FPS/CPU는 7만 19.989/39.22%, 14만 19.972/69.31%, 28만 9.79/71.74%였다. 같은 단계의 새 Area도 28만 평균 18.08 FPS로 낮아졌다. 외부 환경 영향과 실행 간 변동을 분리하지 못했으므로 padding이 단독 원인이라고 단정할 수 없다. 추가 이득이 입증되지 않은 변경을 최종 코드에 남기지 않았다.

모든 세 단계 성능 실행에서 미갱신 Actor는 0이었다. 측정 스크립트는 중복 update 및 종료 시 pending 상태를 검증한다. 성능 실행은 Task 통계 OFF이므로 Task 수명 검증은 별도 통계 ON 검사와 구분한다.

## 판단과 남은 비용

현재 합성 부하에서는 새 Area가 우세하다. radius 방식은 여러 atomic 워드 획득/해제, 획득 실패 rollback, 큐 선두 충돌, 잠금 해제 알림이 남는다. 새 Area와 달리 Area별 타이머 Task를 예약한다. 각 비용의 비중은 이 실험만으로 확정할 수 없다.

한곳 집중 사례는 한 Area의 Actor 순회가 중심이므로 넓은 맵 병렬성 비교와 다르다. 두 방식 모두 약 20 FPS지만, 이 결과로 실제 전투 로직 처리량을 보장하지 않는다. 실제 게임 로직은 제공되지 않았다.

원자료와 실행 파일은 artifacts/legacy-optimization의 comparison(1차), bitmap(최종 채택), padded(추가 후보)에 보관했다. Run-LegacyOptimization.ps1로 동일 시나리오를 다시 실행할 수 있다.

## 최종 검증

- Source/AreaUpdate.sln: MSBuild Release/Debug x64 빌드 성공. 최종 실행 파일은 기본 Task 통계 ON으로 복구했다.
- Release/Debug AreaTests 통과. Release UpdateComparison --test 통과.
- 통계 ON, Actor 5,000명, 5초 이동·이벤트 검사: legacy 균등/집중 모두 overlaps=0, sent=applied, ownership_cycle=1, tasks_live=0.
- Format-Code.ps1 -All -Check(32파일), Check-CodeStyle.ps1, git diff --check 통과.
- reference의 SHA256 목록과 대조하여 새 Area 구현 및 원본 Baseline 파일이 변경되지 않았음을 확인했다.
- 최종 코드와 bitmap 측정 시점의 차이는 빈 줄, 예외 메시지 용어, 미사용 include 정리뿐이며 실행 알고리즘은 같다.
