# Actor 실행 Task 분리와 제출 대기 표본 측정

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

## 1단계: 전용 Task 비교

RuntimeBackend::dispatch_object의 FunctionTask와 캡처 람다를 ObjectUpdateTask로 대체했다.
기존 람다 본문은 update_object로 옮겼다. 전용 Task는 backend/schedule 포인터만 보유한다.
Actor별 TaskExecuter를 그대로 거치며, 직렬 실행·round-robin·재전달 정책은 바꾸지 않았다.
고정 50ms 예약, 종료 확인 위치와 다음 타이머 제출 순서도 유지했다.
매 갱신의 Task 생성·삭제는 남아 있고, 풀이나 Task 재사용은 추가하지 않았다.

기준 실행 파일과 전용 Task 실행 파일을 계측 추가 전에 각각 보존했다.
기준판은 앞선 Dispatcher 전용 Task·relaxed·통계 옵션이 이미 반영된 버전이며 예전 Object 원본이 아니다.
Task 통계 ON, 타이머 상세 추적 OFF, poll, 5ms·256슬롯, 추가 Actor 연산 0이다.
7만은 맵 320×320·이동 시도 2,000회/초, 14만은 453×453·4,000회/초다.
5초 분산 입장과 1ms 입장 확인, seed 42를 사용한다.
각 20초 3회, 순서는 기준→전용 / 전용→기준 / 기준→전용으로 교차했다.
CPU는 입장 완료 후 1초 이후 표본의 시간 가중 평균, FPS는 입장 포함 전체 실행이다.

| Actor | 기준 FPS | 전용 Task FPS | 기준 CPU | 전용 Task CPU |
|---:|---:|---:|---:|---:|
| 70,000 | 19.629 | 19.603 | 88.19% | 87.59% |
| 140,000 | 17.067 | 17.959 | 90.58% | 92.19% |

표는 3회의 중앙값이다. 7만은 사실상 같고 14만은 약 5.2% 높은 FPS가 관측됐다.
14만 기준 범위는 16.777~17.343, 전용 Task는 17.874~18.085FPS였다.
대폭 개선이나 모든 부하에서의 개선이라고 볼 수 없으며 14만은 여전히 목표 20FPS 미달이다.
따라서 남은 지연 구간을 확인하는 2단계 표본 계측도 진행했다.

## 2단계: 표본 계측 설계

예제에만 추가했으며 Core와 TaskDispatcher의 실행 구조는 수정하지 않았다.
ObjectSampleStride=0이 기본 비활성, 256이면 해시로 약 1/256의 Actor를 선택한다.
Actor ID를 단순히 256으로 나누면 timer Worker 8개 중 하나만 고를 수 있어 ID를 혼합했다.
실제 실행에서 모든 timer Worker의 표본 존재와 Worker별 합계 일치를 검사한다.
Worker별 표본은 소속 timer Worker 기준이며 실제 Actor를 실행한 Dispatcher Worker의 부하 균등을 증명하지 않는다.

선택된 Actor에만 통계 저장 공간을 입장 시 한 번 할당한다.
해당 Actor의 TaskExecuter 직렬 실행과 큐의 공개 동기화를 사용하므로,
갱신마다 공유 atomic·로그 mutex·동적 할당·파일 I/O를 추가하지 않는다.
새 계측은 Task 객체에 타임스탬프 필드를 추가하지 않는다.
다만 ObjectSchedule에 표본 포인터와 비활성 검사 분기가 추가되므로,
계측 OFF도 1단계 전용 Task 바이너리와 완전히 같은 바이너리는 아니다.
2단계의 ON/OFF 대조에는 같은 계측 지원 실행 파일을 사용한다.

측정 구간은 다음과 같다.

1. 예정 tick → 실행 Task 제출 직전: 타이머 측 지연. 반올림·등록 지연·만료 확인·실행 지연을 포함한다.
2. 실행 Task 제출 직전 → Actor 콜백 시작: Task 생성·TaskExecuter 제출·Dispatcher 대기·실행 준비를 포함한다.
3. Actor 콜백 진입/복귀 사이: 벽시계 시간이며 계측 시계 호출 비용과 OS 실행 중단도 영향을 줄 수 있다.

2번을 순수 Dispatcher 큐 대기나 특정 mutex 대기라고 해석하면 안 된다.
샘플은 같은 Actor가 순차적으로 기록하고 Core Worker join 및 Dispatcher wait_idle 이후에만 집계한다.
초기 즉시 실행은 제외한다. 나머지는 입장 구간을 포함한다.
타이머 지연과 제출 대기는 1ms histogram, 200ms 이상은 마지막 bucket으로 합친다.
최댓값은 별도로 보관한다. 비활성 분위수는 null(미측정)이며 0ms가 아니다.

## 표본 결과

계측 지원 실행 파일 하나로 7만·14만 각각 stride 0/256을 20초씩 2회 비교했다.
각 Actor 수의 첫 회는 OFF→ON, 다음 회는 ON→OFF다. 상세 타이머 추적은 계속 OFF다.
7만은 312개, 14만은 619개 Actor가 선택됐으며 모든 timer Worker에서 표본을 얻었다.
표는 두 실행에서 관측한 지표의 범위다. 두 실행 전체를 합친 분위수는 아니다.

| 구간/지표 | 7만 | 14만 |
|---|---:|---:|
| 타이머 측 지연 평균 | 15.85~16.05ms | 21.78~30.94ms |
| 타이머 측 지연 p99 | 49~51ms | 56~84ms |
| 제출 후 실행 대기 평균 | 0.752~0.795ms | 9.126~13.324ms |
| 제출 후 실행 대기 p99 | 8~9ms | 33~49ms |
| Actor 콜백 벽시계 평균 | 0.352~0.357us | 0.495~0.511us |

이 추가 연산 0 예제에서는 Actor 콜백보다 앞선 타이머/전달 경로 지연이 훨씬 컸다.
14만에서 제출 후 대기도 커지므로 타이머 측 하나만으로 모든 지연을 설명할 수 없다.
Actor 콜백 최대는 약 2.31ms까지 관측돼 OS 실행 중단 등의 꼬리 지연도 포함한다.
이 자료는 CPU 프로파일이 아니므로 특정 atomic·allocator·mutex·OS 스케줄링 중
어느 항목이 CPU 병목인지는 확인 불가다. Actor 간 상호작용이 많은 게임으로 일반화하지 않는다.

### 계측 OFF 대조

| Actor | OFF FPS (2회) | ON FPS (2회) | OFF CPU (2회) | ON CPU (2회) |
|---:|---|---|---|---|
| 70,000 | 19.733 / 19.587 | 19.822 / 19.750 | 86.71 / 87.24% | 87.11 / 87.30% |
| 140,000 | 17.737 / 14.045 | 17.809 / 14.421 | 90.93 / 76.84% | 90.37 / 78.01% |

14만은 두 번째 회에서 OFF도 ON도 함께 낮아졌다. 실행 간 환경 변동이 커
정확한 계측 비용 비율을 확정할 수 없으며, 계측이 FPS를 개선했다고 해석하지 않는다.
비활성은 기본값으로 유지한다. 표본 기록을 끄더라도 계측 지원 필드/분기 자체가 사라지는 빌드는 아니다.

## 재현과 검증

```powershell
# 1단계: 보존한 기준/전용 Task 실행 파일로 비교
.\Run-ObjectOverheadComparison.ps1 -Variants baseline,dedicated -Repetitions 3 -BinaryDirectory artifacts/object-actor-task -OutputDirectory artifacts/object-actor-task-repeat
# 2단계: 보존한 계측 지원 실행 파일로 stride 0/256 대조
.\Run-ObjectDispatchProfile.ps1 -OutputDirectory artifacts/object-dispatch-repeat
# 마지막 인수 256이 표본 비율. 생략/0이면 비활성
.\Source\x64\Release\AreaUpdateExample.exe 20 140000 4000 42 5000 1 0 453 job 0 poll 256
```

자료는 artifacts/object-actor-task 아래에 저장한다.
baseline/dedicated/profiled는 각 실행 파일과 소스, measurements는 1단계 결과,
profiling은 2단계 결과다. 두 단계의 FPS를 하나의 비교 집합으로 혼합하지 않는다.
각 실행의 로그·CPU 표본·summary.json과 실행 파일 SHA-256을 보존한다.

추가 회귀 검사는 실제 RuntimeBackend의 반복 갱신·중복 방지·종료 시 Task 정리,
집계 합계·histogram 포화·timer Worker 표본 분산·finish 이전 조회 거부를 확인한다.
Release/Debug 솔루션 빌드와 기존 소유권·직렬화·round-robin·상호작용 checksum 검사를 통과했다.
1단계 12회와 2단계 8회 모두 종료 코드 0, 중복·미갱신 Actor·잔여 Task 0이었다.
계측 ON의 Worker별 표본 합계와 전체 합계도 일치했다.
