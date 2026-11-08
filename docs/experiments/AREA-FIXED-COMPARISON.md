# Area 고정 주기 예약과 Object 비교

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

## 변경

AreaExecuter의 갱신 완료 후 +50ms 상대 예약을 이전 예정 시각 +50ms 고정 예약으로 변경했다.
Core/UpdateTickPlan.h로 계산을 공통화했다. 당시 추가한 ObjectTickPlan.h 호환 래퍼는
후속 정리에서 제거했으며 현재 Area와 Object 모두 공통 함수를 직접 호출한다.
Object의 기존 고정 주기 의미는 바꾸지 않았다.

- 예약 토큰을 새로 얻은 Area만 기준 시각을 초기화하고 큐에 공개한다.
- 예정 시각은 해당 Area의 스케줄러 소유자만 갱신한다. 큐 공개/수신으로 다음 소유자에게 전달한다.
- 예정 시각이 1,000ms이고 완료가 1,017ms이면 다음은 1,050ms다.
- 완료가 1,173ms이면 이미 지난 3개 슬롯을 건너뛰고 1,200ms로 예약한다.
- Worker::add_task_at으로 절대 시각을 전달한다. 과거 tick을 연속으로 따라잡지 않는다.
- 비활성으로 예약 체인이 끝난 뒤 다시 활성화하면 새 시각에서 시작한다.
- 이미 예약 중인 Area에 activate가 다시 들어오는 것만으로 기준을 재설정하지 않는다.

추가한 m_nextUpdateTick은 예약 소유자 또는 Worker join 이후에만 읽어야 한다.
AreaSkippedTicks는 batch별로 합쳐 기록한다. Actor별 ObjectSkippedTicks와 직접 비교하는 수치가 아니다.
partition 반경 2·폭 5·25개 색상, batch 64, admission 8, 공평성, Actor 소유권/이동 처리는 유지했다.
고정 예약은 시간 기준 변경이지 공간 실행 제한을 바꾼 것이 아니다.
Source/AreaUpdateLegacy/Baseline과 OriginalAreaAdapter는 수정하지 않았다.

## 비교 구성

변경 직전 실행 파일을 before로 보존하고, 변경 후 실행 파일을 after로 보존했다.

| 구분 | 실행 파일 | 실행 모드 | 예약 |
|---|---|---|---|
| relative-area | before | area | 완료 후 +50ms |
| fixed-area | after | area | 예정 시각 기준 50ms |
| object | after | job | 예정 시각 기준 50ms |

relative-area는 이번 변경 직전의 현재 Area이며 Git의 예전 AreaUpdateLegacy가 아니다.
고정 Area와 Object는 같은 실행 파일에서 비교하므로 공통 Core와 진단 설정이 같다.
Area는 Core Worker 8개, Object는 Core Worker 8개 + Dispatcher Worker 8개다.
논리 CPU 8개 기준의 전체 프로세스 사용률을 측정한다.

Actor 1만·7만·14만·28만, 각 조건 20초씩 2회다.
첫 회는 relative-area→fixed-area→object, 다음 회는 역순으로 실행한다.
seed 42, 5초 분산 입장, 입장 확인 1ms, Actor 추가 연산 0이다.
맵과 이동 시도는 아래와 같이 같은 Actor 수에서 동일하게 맞춘다.

| Actor | 맵 한 변 | 이동 시도/초 |
|---:|---:|---:|
| 10,000 | 121 | 286 |
| 70,000 | 320 | 2,000 |
| 140,000 | 453 | 4,000 |
| 280,000 | 640 | 8,000 |

5ms·256슬롯, poll, Task 생성/소멸 통계 ON을 유지한다.
타이머 상세·Actor 제출·Dispatcher 계측은 전부 OFF다.
CPU는 전체 논리 CPU 기준이고 입장 완료 후 1초 이후 표본의 시간 가중 평균이다.
Actor FPS는 입장 시점 이후 시간 합계로 나눈 전체 실행 기준이다.

## 해석 범위

예약 정책은 같아졌지만 업데이트 단위는 다르다.
Area는 Actor 목록을 한 번에 처리하고 이동을 pending 등록/탈퇴로 반영한다.
Object는 Actor별 예약/실행기를 사용하며 예제의 이동은 위치/점유 메타데이터 처리다.
Area를 이동한 Actor는 목적지의 예약 위상을 따르므로 Actor별 간격이 항상 50ms인 것은 아니다.
이 예제에는 Actor 간 상호작용과 추가 연산 부하가 없어 실제 게임의 전체 성능으로 일반화하지 않는다.

Object 모드도 공통 Area 그리드를 할당하므로 최소 메모리 구현끼리의 비교가 아니다.
Area에 예정 시각 필드가 추가돼 before/after의 Area 크기도 다르다.
작은 성능 차이는 이 구조 차이와 실행 환경 변동을 고려해야 한다.
정확한 예약 계산·위상 유지 여부는 별도 회귀 테스트로 검증한다.

## 결과

24회 모두 실행을 완료했다. 표는 각 조건 2회 지표의 중앙값이며 두 값의 중앙값은 산술평균과 같다.
각 실행의 FPS 분포를 합친 값은 아니며 반복 수가 적으므로 작은 차이의 우열은 단정하지 않는다.

| Actor | 기존 Area FPS | 고정 Area FPS | Object FPS | 기존 Area CPU | 고정 Area CPU | Object CPU |
|---:|---:|---:|---:|---:|---:|---:|
| 10,000 | 15.75 | 20.00 | 20.02 | 2.38% | 2.97% | 29.94% |
| 70,000 | 15.17 | 19.51 | 19.26 | 10.91% | 14.32% | 85.84% |
| 140,000 | 11.52 | 11.81 | 17.92 | 15.97% | 16.63% | 92.10% |
| 280,000 | 5.68 | 5.75 | 9.13 | 16.04% | 16.67% | 91.52% |

### 예약 변경 효과

- 1만은 약 27.0%, 7만은 약 28.6% 높은 FPS가 관측됐다. 완료 후 지연을 다음 주기에 계속 더하던 효과가 줄었다.
- 7만 고정 Area는 19.015/19.999FPS, Object는 18.814/19.707FPS였다. FPS의 작은 우열보다 CPU 차이가 더 뚜렷했다.
- 14만 고정 Area는 11.758/11.865FPS로, 기존 11.569/11.469FPS와 큰 차이가 없었다.
- 28만 고정 Area는 5.719/5.788FPS, 기존은 5.604/5.754FPS로 범위가 겹친다. 처리량 개선을 확정하기 어렵다.

따라서 고정 예약은 저부하의 주기 드리프트를 해결하지만 큰 부하의 Area 처리 한계를 해결하지는 못했다.
이번 예제에서 7만까지는 두 방식의 FPS가 비슷하고 Area의 CPU가 낮았다.
14만 이상은 Object가 더 높은 FPS를 처리했지만 CPU도 약 92% 사용했다.
Area는 약 16~17% CPU에서 목표 FPS를 달성하지 못하므로 현재 구현에 별도 처리 제한이 남아 있다.
이 결과를 모든 게임에서 Object가 낫다거나 Area 구조 자체의 최대 성능이라고 일반화하지 않는다.

### 남은 제한에 대한 관측

14만 고정 Area 첫 실행에서 wait 11,640회 중 blocked_samples는 11,310회였다.
이 값은 실행할 Area가 남아 있지만 현재 partition의 admission 예산이 소진돼 runnable=false인 시점의 표본이다.
반경/공평성 때문에 다른 partition으로 진행하기 전 기존 runner들이 끝나기를 기다리는 구조는 그대로다.
이 관측은 대기/스케줄링 경로의 추가 확인이 필요하다는 근거지만,
CPU 프로파일이나 OS 실행 배정 추적이 아니므로 특정 mutex나 sleep 하나가 원인이라고 확정하지 않는다.
이번 작업에서는 대기 방식·admission·partition 정책을 추가 수정하거나 다른 설정 결과를 섞지 않았다.

### 메모리 표본

고정 Area/Object의 최대 private memory 표본은 7만 약 122.8/137.4MB,
14만 약 234.1/270.3MB, 28만 약 447.9/520.8MB였다.
종료 시점 live object 크기나 최소 메모리 구현을 뜻하지 않으며, 전체 할당 추적 결과도 아니다.

## 검증과 재현

80ms 지연을 넣어 지난 예약 건너뛰기, 최초 위상 유지, 조기 실행 방지,
비활성 예약 종료 이후 재활성화 기준 초기화를 실제 AreaExecuter로 검증한다.
공통 계산과 Object 래퍼의 일치 및 지연 17ms가 1,000번 반복돼도 드리프트가 없는지 확인한다.
기존 partition 배제·병렬 batch·Actor 소유권·round-robin·상호작용 checksum 검사는 유지한다.
Release/Debug 솔루션 빌드와 새 회귀 테스트를 통과했다.
24회 모두 종료 코드 0, 중복 갱신·미갱신 Actor·잔여 Task 0을 확인했다.

```powershell
# 보존된 before/after 실행 파일로 새 결과 폴더에서 비교
.\tools\experiments\Run-FixedAreaComparison.ps1 -OutputDirectory artifacts/area-fixed-repeat
# 현재 코드에서 당시 조건을 재현: 두 모델 모두 고정 예약 + poll 명시
.\Source\x64\Release\AreaUpdateExample.exe 20 70000 2000 42 5000 1 0 320 area 0 poll
.\Source\x64\Release\AreaUpdateExample.exe 20 70000 2000 42 5000 1 0 320 job 0 poll
```

artifacts/area-fixed-comparison/before와 after에 실행 파일·핵심 소스를 보관한다.
measurements에 SHA-256, 환경, 메모리 안전 계획, 결과 CSV와 실행별 로그·CPU 표본·JSON을 보관한다.
