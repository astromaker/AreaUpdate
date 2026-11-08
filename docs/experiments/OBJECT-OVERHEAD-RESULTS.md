# ObjectUpdate 공통 비용 개선

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

## 변경

### 1. Task 통계 비용 분리

Task 생성·소멸마다 전역 atomic을 각각 2회씩 갱신하던 코드를 빌드 옵션으로 제외할 수 있게 했다.
기본값은 활성이다. 비활성 빌드는 Task 통계 갱신과 통계 저장 객체를 컴파일에서 제외한다.
Task 객체 크기를 바꾸거나 실행 시 조건 분기를 추가하지 않는다.

MSBuild의 TaskStatisticsEnabled=0이 AREAUPDATE_TASK_STATISTICS=0을 모든 프로젝트에 전달한다.
CMake에도 같은 이름의 AREAUPDATE_TASK_STATISTICS 옵션을 추가했으며 기본 ON이다.
하나의 실행 파일을 구성하는 모든 Task 사용 코드가 같은 설정이어야 한다.
서로 다른 설정으로 빌드한 정적 라이브러리를 혼합하면 안 된다.
이번 실제 빌드 검증은 요청대로 Source/AreaUpdate.sln과 MSBuild로 수행했다.

통계가 없으면 Task::statistics()는 빈 값을 반환하지만 예제는 비활성 상태를 명시한다.
측정 스크립트는 tasksCreated/tasksDestroyed/tasksLive를 null로 기록하고
taskLifetimeVerified=false로 남긴다. 0개 생존을 확인한 것으로 취급하지 않는다.
Worker pending, Actor 중복·미갱신 검사는 비활성 빌드에서도 유지된다.
공통 Core 변경이므로 AreaUpdate에도 같은 옵션이 적용된다.

### 2. Dispatcher 전달 Task 단순화

기존 TaskExecuter::post_execute는 shared_ptr를 캡처한 람다를 std::function으로 만들고,
TaskDispatcher::post는 다시 람다와 FunctionTask로 감쌌다.
이 경로를 shared_ptr<TaskExecuter> 하나를 보유한 ExecuteTask로 대체했다.
공개 post(std::function<void()>)도 추가 람다 포장 없이 FunctionTask로 전달한다.

새 post(unique_ptr<Task>)는 소유권을 이전받고 nullptr은 예외로 거부한다.
완료 카운터 감소는 개별 람다가 아니라 Worker의 실행·삭제 이후 한 곳에서 수행한다.
따라서 wait_idle은 Task 소멸까지 완료된 뒤 반환한다.
기존에는 함수 본문 안에서 완료 카운터를 감소시켜 삭제보다 먼저 반환할 수 있었다.

전용 전달 Task 역시 매번 생성·삭제한다. 풀을 추가하지 않았고,
타이머/Actor/Dispatcher Task 세 종류의 생성 자체를 없앤 것은 아니다.
std::function 포장을 제거했지만 전체 힙 할당 횟수를 별도 계측하지 않았으므로
정확히 몇 회의 힙 할당을 절감했다고 단정하지 않는다.

### 3. Worker 선택 카운터의 순서 완화

m_nextWorker.fetch_add만 memory_order_relaxed로 바꿨다.
번호가 유일하게 증가하는 atomic 수정 순서는 유지돼 기존 전역 round-robin 배정은 그대로다.
큐 데이터 공개는 TaskQueue의 release/acquire가 담당하므로 선택 카운터에 의존하지 않는다.
m_pendingTasks, 실행권 카운터, 재전달, 대기 알림의 동기화는 완화하지 않았다.
공유 카운터의 존재나 캐시 일관성 비용 자체를 없앤 변경은 아니다.
캐시라인 배치를 확인하지 않은 padding이나 새로운 Worker 분배 정책은 추가하지 않았다.

## 비교 방법

변경 전 Release 실행 파일을 먼저 보존하고 다음 다섯 실행 파일을 만들었다.

| 버전 | Task 통계 | Dispatcher 전달 | Worker 선택 |
|---|---|---|---|
| baseline-on | 활성 | 기존 중첩 함수 | 기존 |
| baseline-off | 비활성 | 기존 중첩 함수 | 기존 |
| wrapper-on | 활성 | 전용 Task | 기존 |
| final-on | 활성 | 전용 Task | relaxed |
| final-off | 비활성 | 전용 Task | relaxed |

Object(job), Actor 7만·14만, 각 20초씩 2회, seed 42, 추가 연산 0이다.
첫 회에는 표 순서, 다음 회에는 역순으로 실행한다.
7만의 맵은 320×320, 이동 시도 2,000회/초,
14만은 453×453, 이동 시도 4,000회/초다.
5초 분산 입장, 1ms 입장 확인, 고정 50ms 예약,
5ms·256슬롯, poll 대기, 타이머 지연 추적 비활성을 공통으로 유지한다.
타이머 Worker 8개와 Dispatcher Worker 8개, 논리 CPU 8개 구성이다.
CPU는 입장 완료 후 1초 이후의 시간 가중 평균이고 FPS는 입장 구간을 포함한 전체 실행 기준이다.
진단 통계를 끈 빌드에도 Worker 통계와 Actor별 검사는 남아 있으므로 완전한 무계측 빌드는 아니다.

이번 비교는 Area와 Object의 우열 시험이 아니라 Object 비용 항목의 분리 비교다.
현재 Area 코드와 Object의 주기 예약 방식 차이도 수정하지 않았다.

## 결과

아래 값은 각 조건 2회 지표의 중앙값이다. 두 값의 중앙값은 산술평균과 같다.
총 20회 모두 종료 코드 0, 중복·미갱신 Actor·Worker pending 0이었다.
통계 활성 실행은 Task 생존 수 0을 확인했고 비활성 실행은 미측정으로 남겼다.

| 버전 | 7만 FPS | 7만 CPU | 14만 FPS | 14만 CPU |
|---|---:|---:|---:|---:|
| baseline-on | 18.70 | 88.44% | 14.92 | 90.15% |
| baseline-off | 19.07 | 88.23% | 15.53 | 92.33% |
| wrapper-on | 19.18 | 84.04% | 17.47 | 93.12% |
| final-on | 18.27 | 79.48% | 17.43 | 93.06% |
| final-off | 18.77 | 80.17% | 17.53 | 91.41% |

### 해석

- 1번: 통계만 제외한 기준판은 7만에서 약 2.0%, 14만에서 약 4.1% 높은 FPS였다.
  14만 CPU도 함께 늘었으며, 반복 수가 적어 이를 통계의 고정 비용 비율이라고 단정하지 않는다.
- 2번: 전용 전달 Task를 쓴 wrapper-on은 14만에서 약 17.1% 높은 FPS였다.
  최종 final-on도 기준판보다 약 16.8% 높았으므로 해당 부하에서 개선이 관측됐다.
- 3번: 현재 x64 Release의 wrapper-on과 final-on은 PE .text 섹션 73,728바이트가 동일했다.
  SHA-256은 둘 다 6E59C673F4B1725B70F5A5351CE2235D3BE2DC10D322E2D74282418A012CCF0A다.
  따라서 이 빌드에서 relaxed 변경 자체의 별도 실행 코드 개선은 없으며 측정 차이를 그 효과로 해석하지 않는다.
- 7만 final-on은 17.042와 19.495FPS로 편차가 컸다. wrapper-on은 19.274와 19.090FPS였다.
  동일한 실행 코드 사이에서도 차이가 발생했으므로 7만의 일관된 개선·회귀를 확정하기 어렵다.
- 14만 final-on은 17.445와 17.409FPS였다. 개선은 관측됐지만 목표 20FPS를 달성한 것은 아니다.
- 14만 최대 private memory 표본은 기준 통계 활성에서 약 290.5MB, 최종 통계 활성에서 약 268.4MB였다.
  전체 프로세스 표본이지 Task별 할당 추적이 아니므로 절감 원인을 정확한 할당 횟수로 환산하지 않는다.

측정은 전용 장비가 아닌 환경에서 각 2회 수행했다. 작은 차이를 확정적 우열로 보지 않는다.
Actor 추가 연산이 없고 이 예제에는 Actor 간 상호작용 부하가 없으므로 실제 게임 성능으로 일반화하지 않는다.
기본 진단 설정은 활성으로 유지한다. 이후 Area와 비교할 때는 양쪽의 Task 통계 설정을 같게 맞춰야 한다.

## 검증과 재현

통계 활성·비활성 모두 Release/Debug 솔루션 빌드와 AreaTests, UpdateComparison --test를 통과했다.
추가한 Dispatcher 완료 검사는 Task 소멸이 끝나기 전 wait_idle이 반환하지 않는지,
외부 참조를 해제한 실행기가 작업을 마치고 소멸하는지, null Task를 거부하는지 확인한다.
기존 4개 생산자의 실행 직렬화, busy Worker를 건너뛰지 않는 round-robin,
재전달 및 상호작용 checksum 검사도 유지한다.

```powershell
# 통계 제외 빌드. Core를 포함한 솔루션 전체에 같은 옵션을 전달한다.
MSBuild Source/AreaUpdate.sln /m /p:Configuration=Release /p:Platform=x64 /p:TaskStatisticsEnabled=0
# 기본 진단 빌드 복원
MSBuild Source/AreaUpdate.sln /m /p:Configuration=Release /p:Platform=x64 /p:TaskStatisticsEnabled=1
# 보존한 다섯 실행 파일로 비교. 새로운 결과 폴더를 지정한다.
.\Run-ObjectOverheadComparison.ps1 -BinaryDirectory artifacts/object-overhead -OutputDirectory artifacts/object-overhead-repeat
```

실행 파일·핵심 소스·SHA-256·환경·실행별 로그·CPU 표본·요약을
artifacts/object-overhead에 보존한다.
