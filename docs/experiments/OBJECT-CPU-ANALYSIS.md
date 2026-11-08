# Object 모델의 높은 CPU 사용 원인

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

## 결론

현재 예제에서는 짧은 Actor 갱신마다 개별 타이머·실행 작업·Dispatcher 전달을 반복하는 비용이 크다.
Area처럼 여러 갱신을 묶는 모델보다 작업 관리가 훨씬 잦다.
특정 Worker에 작업이 몰리거나 빈 전달 작업이 폭증하는 것이 이번 측정의 주원인이라는 증거는 없었다.
다만 할당·atomic·타이머·깨우기 각각의 CPU 기여율은 이번 계측으로 분리하지 못했다.

## 확인한 실행 경로

`Source/Examples/AreaUpdateExample/RuntimeBackend.h`의 `dispatch_object()`와 `update_object()`:

```text
Core Worker가 ObjectTimerTask 만료 처리
  → new ObjectUpdateTask
  → Actor 전용 TaskExecuter 큐에 제출
  → new ExecuteTask + shared_ptr로 실행기 수명 유지
  → TaskDispatcher가 전역 round-robin으로 실행 Worker 선택
  → Dispatcher Worker가 Actor::update 실행
  → new ObjectTimerTask를 원래 timer Worker에 등록
  → 다음 갱신까지 반복
```

따라서 정상 주기에서 갱신당 Task 약 3개를 생성·삭제한다.
Actor 최초 입장·종료 때의 취소 작업 등으로 실측 비율은 정확히 3이 아닌 약간 큰 값이다.
Actor 간 상호작용이 없는 이 예제는 실행기 내부 작업을 여러 개 모아 처리하는 이점도 거의 활용하지 못한다.

## 실제 작업 수와 비용

이전 최신 3모델 비교의 seed 42 실행을 사용했다. 동일한 EXE이며 아래 CPU 비용은 전체 실행 프로세스 CPU 시간을
실제 Actor 갱신 수로 나눈 상각 비용이다. Actor 본문만의 CPU 시간이나 정상 구간 전용 수치는 아니다.

| Actor | 모델 | Actor 갱신 수 | Task 생성 수 | 갱신당 Task | 갱신당 프로세스 CPU |
|---:|---|---:|---:|---:|---:|
| 70,000 | Area | 27,311,726 | 14,601,934 | 0.535 | 1.629µs |
| 70,000 | Area(old) | 26,612,058 | 4,783,928 | 0.180 | 2.031µs |
| 70,000 | Object | 26,501,023 | 79,573,924 | 3.003 | 5.108µs |
| 280,000 | Area | 101,926,477 | 44,719,943 | 0.439 | 1.359µs |
| 280,000 | Area(old) | 77,497,316 | 7,470,133 | 0.096 | 1.652µs |
| 280,000 | Object | 44,272,024 | 133,445,219 | 3.014 | 3.296µs |

Task 생성 수는 전체 힙 할당 수와 같지는 않다. 다른 컨테이너 등의 할당은 이 수치에 포함되지 않는다.
낮은 갱신률의 실행에서는 덜 갱신한 Actor가 늘어나므로 CPU 사용률만 단독 비교하면 안 된다.

## 비용이 붙는 위치

### 1. 할당·삭제와 3단계 큐 전달

- `RuntimeBackend.h`: 매 주기 `ObjectTimerTask`, `ObjectUpdateTask` 생성.
- `TaskExecuter.cpp::post_execute()`: 매 실행권 전달에 `ExecuteTask` 생성.
- `TaskExecuter.cpp::run_task()/execute()`: Actor별 큐 공개·분리·실행권 카운터 증감.
- `TaskDispatcher::post()/run_worker()`: Dispatcher 큐 공개·분리, 전달 작업 삭제.
- `Worker::submit()/run_task()/run_timer()`: timer Worker 요청 큐·타이머 휠 등록과 만료 작업 삭제.

이 비용은 Actor 본문이 짧아도 생긴다. Area 쪽의 타이머·예약 batch는 여러 갱신에 비용을 분산한다.

### 2. 전역 atomic 및 계측 비용

`Source/Core/Task.h`는 기본 설정에서 Task 생성에 전역 atomic 2회, 소멸에 2회를 갱신한다.
7만 Object의 위 실행에서는 이 통계만 약 3억 1,830만 회의 atomic 증감을 수행했다.
이는 CPU 샘플 비율이 아니라 코드와 생성/소멸 건수로 계산한 연산 횟수다.

여기에 Dispatcher 전역 `m_nextWorker`, `m_pendingTasks`, Actor별 실행권 카운터,
각 timer Worker의 pending/depth/submitter 카운터와 큐 CAS가 추가된다.
전역 round-robin의 relaxed 지정도 공유 카운터 자체를 없애지는 않는다.
실제 캐시 미스·cache-line bouncing 비중은 별도 하드웨어/CPU 프로파일 없이 확정할 수 없다.

이전 `OBJECT-OVERHEAD-RESULTS.md`에서는 통계 제거만으로 모든 비용이 사라지지 않았다.
그때는 poll·5초 입장 등 조건이 달랐으므로 과거 개선율을 이번 signal 실행에 그대로 적용하지 않는다.

### 3. 타이머와 실행기 사이의 스레드 전달

타이머 Worker 8개와 Dispatcher Worker 8개가 논리 CPU 8개를 공유한다.
각 Actor의 다음 타이머는 원래 timer Worker로 돌아가고 실제 실행은 전역 round-robin으로 선택된다.
따라서 하나의 Actor 갱신만 보더라도 풀 사이 전달이 발생하고 실행 Worker 고정 affinity는 보장되지 않는다.
16개 스레드라는 사실만으로 높은 CPU나 과도한 context switch가 증명되는 것은 아니다.
이번에는 스레드별 CPU·context switch·캐시 비용을 직접 분리하지 않았다.

### 4. 상세 추적 OFF에도 남는 공통 진단

`Worker::submit()`의 제출 시각 기록, 큐 깊이/최댓값 관리,
`run_task()`의 큐 지연과 `run_timer()`의 만료 지연 측정은 상세 trace OFF에도 실행된다.
`WheelTimer::expired_tasks()`는 결과 vector를 만들고, 결과가 있으면 256슬롯의 다음 만료를 재검색한다.
이 검색은 Actor마다가 아니라 만료 추출 호출마다 수행된다. 단독 주범이라고 단정하지 않는다.
Actor 본문에도 시각 조회·중복 실행 검사·갱신 간격 histogram이 있어 완전히 무계측인 빈 함수는 아니다.

## 최신 실행 파일로 추가 계측

기존 EXE SHA256: `882C22FF194AF67CA7E4245D28B8B388AF0116265BC19D4080D682727A169356`.
20초, seed 42, 1초/1ms 분산 입장, signal, 추가 Actor 연산 0.
7만은 OFF→ON, 28만은 ON→OFF로 각 1회씩 실행했다.
ON은 ObjectSampleStride=256, DispatcherProfileStride=256이고 다른 정책은 바꾸지 않았다.

| Actor | 계측 | 정상 FPS | CPU |
|---:|---|---:|---:|
| 70,000 | OFF | 18.39 | 78.00% |
| 70,000 | ON | 19.61 | 82.27% |
| 280,000 | ON | 8.02 | 86.15% |
| 280,000 | OFF | 8.04 | 87.63% |

1회씩이고 7만에서 관측 변동이 커 계측 오버헤드의 정확한 비율을 추정하지 않는다.
특히 ON의 FPS가 높았다는 이유로 계측 기능이 성능을 개선한다고 해석하면 안 된다.

| 계측 항목 | 7만 | 28만 |
|---|---:|---:|
| Actor 본문 평균 벽시계 | 0.377µs | 0.671µs |
| 타이머 dispatch → Actor 시작 평균 대기 | 1.656ms | 66.450ms |
| 같은 구간 P99 histogram | 약 14ms | 200ms 이상 마지막 구간 |
| 예약 시각 → timer dispatch 평균 지연 | 17.823ms | 28.832ms |
| Worker 최대/평균 전달 건수 | 약 1.000배 | 약 1.000배 |
| Worker 최대/평균 batch 실행 벽시계 | 1.051배 | 1.141배 |
| Dispatcher 전달 Task / 실행기 내부 Task | 26,537,170 / 26,537,170 | 42,604,838 / 42,604,838 |

Actor 시간은 표본 벽시계이며 중단·시계 조회 비용도 포함한다. 프로세스 CPU 대비 비율로 나누지 않는다.
평균 대기 시간은 CPU 소비 시간이 아니라 큐 적체 지표다.
건수 균등은 round-robin 동작을 보여주지만 모든 순간의 부하 균등을 보장하지 않는다.
전달/내부 Task 건수가 같아 이번 실행에서 빈 전달 작업의 증폭은 관측되지 않았다.
두 풀의 누적 busy 시간도 CPU 시간이 아니므로 프로세스 CPU와 합산하지 않는다.

## 다음 개선을 한다면

1. Actor별 3개 Task 수명과 풀 사이 왕복을 줄이는 예약·전달 경로를 독립 실험한다.
   실제 Actor 실행 직렬화와 기존 round-robin 재전달 계약을 먼저 보존해야 한다.
2. 실제 서비스용 통계 OFF 빌드를 같은 조건에서 비교해 전역 진단 비용을 분리한다.
   비교 시 Area에도 같은 설정을 적용하고 Task 수명 검증 불가 상태를 명시해야 한다.
3. 이후에도 필요하면 최신 Object의 CPU 스택을 수집해 할당·atomic·타이머·깨우기 비중을 구분한다.
   현재 자료만으로 각 항목이 CPU의 몇 %인지 말할 수는 없다.

이번 요청에서는 분석과 기존 계측만 수행했다. 실행 정책·C++ 코드는 수정하지 않았고,
오브젝트 풀·idle Worker 관리·stealing도 추가하지 않았다.
추가 4회 모두 종료 코드 0, neverUpdated=0, tasksLive=0이었다.
원시 자료는 `artifacts/object-cpu-analysis/results.csv`와 각 실행 폴더에 보존했다.
