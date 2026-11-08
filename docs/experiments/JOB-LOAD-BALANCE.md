# Job Worker 부하 불균형 확인

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

## 코드에서 확인한 경로

- `TaskDispatcher::post()`는 공통 증가 카운터로 round-robin 배정한다. queue 길이, 실행시간, Worker의 현재 부하를 보지 않는다.
- `run_worker()`는 자신의 queue를 `pop_all()`로 분리하고 전부 실행한다. 분리한 목록은 다른 Worker가 가져올 수 없다.
- 현재 최종 구현에는 idle stealing이 없다. 앞선 실험에서는 다른 queue의 `pop_all()`을 사용했으므로 공용 queue에 남은 작업만 가져올 수 있었고, 가져온 목록 전체가 한 Worker에 귀속됐다. 따라서 해당 실험도 작업 시간의 균등 분배를 보장하지 않는다.
- `TaskExecuter::execute()`도 Actor mailbox의 분리 목록 전체를 실행한다. 한 번의 turn이 한 batch이지만 batch 안의 Job 개수나 소요시간에는 상한이 없다.
- 중첩 mailbox의 첫 실행은 TLS로 이어서 실행할 수 있다. 이런 실행은 dispatcher round-robin 배정 횟수에 나타나지 않는다.

즉 배정한 작업 수가 비슷해도 실제 Worker 사용시간은 크게 달라질 수 있다.

## Server 원본의 능동 재분배

`D:/github/Server/server3/Library/Core/PWJobExecuter.cpp`도 확인했다. 원본은 작업이 최초 등록돼 pending count가 0→1이면 실행 소유권을 확보한다. 스레드 그룹이 다르면 `throwToAnyThread()`를 호출하며, 현재 스레드가 다른 JobExecuter를 처리 중이면 `RunNext()`를 사용한다.

`RunNext(executer, false)`는 TLS 대기 목록이 비어 있지 않을 때 `throwToAnyThread()`로 재전달하고, 그렇지 않으면 TLS 목록에 둔다. 이미 전달된 요청은 `tryExecute(true)`로 들어와 다시 무한히 전달하지 않고 TLS에 수용할 수 있다. 한 detached batch 후 Job이 남아도 `RunNext(this, false)`로 재분배 기회를 준다.

현재 이식본도 다른 dispatcher에서 진입한 0→1 요청을 `post_execute()`로 넘기고, 중첩 실행에서 TLS 슬롯이 차면 넘긴다. batch 후 남은 Job은 항상 `post_execute()`로 넘긴다는 점은 원본보다 더 적극적인 재전달이다. 원본은 이 경우 TLS 목록이 비어 있으면 같은 스레드에 둘 수 있다. 따라서 두 구현은 재분배 의도는 공유하지만 세부 정책이 완전히 같지는 않다.

원본 `PWWorkers::pushJobToAny()`도 증가 인덱스의 modulo로 Worker를 고른다. 실제로 한가한 Worker를 탐색하거나 현재 Worker를 제외하지 않으므로 반드시 다른 스레드로 전달된다고 보장하지 않는다. 동일 Actor의 소유권 선출과 재전달은 편중을 완화하지만, 이미 분리한 batch 안의 긴 실행이나 Worker queue의 대기 문제를 모두 제거하지는 않는다.

## 결정적 재현 테스트

`UpdateComparison.exe --test`에 추가했다. 4개 Worker의 dispatcher에 순서대로 다음 작업을 보낸다.

```text
Worker 0: [gate에서 대기하는 작업] -> [다른 독립 작업]
Worker 1: [즉시 완료]
Worker 2: [즉시 완료]
Worker 3: [즉시 완료]
```

다른 Worker 3개의 완료를 확인한 시점에도 Worker 0 뒤의 독립 작업은 실행되지 않아야 현재의 불균형을 재현한 것이다. 이후 gate를 해제하고 모든 작업이 완료되는지도 확인한다. 이 테스트는 현상의 존재를 확인하며 실제 게임 부하에서 발생 빈도나 CPU 편차를 정량화하지는 않는다.

## Actor 집중과 구분

위 gate 시험은 dispatcher의 Worker 대기열을 직접 시험하며, TaskExecuter 재전달 정책이 무효라는 시험은 아니다.

여러 독립 Actor의 작업이 한 Worker 뒤에 대기하는 경우는 스케줄링으로 개선할 수 있다. 반면 한 Actor의 mailbox에 Job이 몰리면 해당 Actor 상태의 직렬화 규칙 때문에 여러 Worker가 동시에 실행하면 안 된다. idle stealing으로 실행 Worker를 바꿀 수는 있지만 같은 Actor의 병렬성 자체가 늘지는 않는다.

## 개선 조건

사용자의 설계 의도는 한가한 Worker를 실시간으로 추적하지 않고 임의 Worker 재전달로 분산하는 것이다. 이 의도를 유지하며 idle 관리나 stealing을 기본 요구사항으로 두지 않는다. 아래 계측은 문제 발생 여부를 확인하기 위한 진단이지 한가한 스레드 관리자를 추가하자는 의미가 아니다.

추후 적용한다면 Worker별 실행시간·작업 수·idle 시간·최대 대기시간을 먼저 기록하고, 공용 queue의 제한된 작업만 가져오는 방식을 비교한다. 전량 가져오기보다는 작은 단위의 stealing과 회전하는 victim 선택을 검토해야 한다. Actor mailbox는 실행 소유권을 유지한 상태에서 제한된 Job 수 또는 시간 예산 이후 재스케줄하는 방식이 후보지만, 이미 분리한 나머지 목록과 동시 producer의 순서를 보존하도록 설계해야 한다.

앞선 묶음 최적화에서 회귀가 관찰되어 stealing은 최종 코드에 적용하지 않았다. 이것은 모든 형태의 work stealing이 부적합하다는 증거는 아니다.

## 검증 결과

Release/Debug 솔루션 빌드 후 `--test`에서 `LOAD_BALANCE_TEST: 3 Workers completed; queued task waited for busy Worker`와 `SELF_TEST passed`를 확인했다. 반례는 존재하지만, 실제 비교 부하에서 Worker별 사용률 편차를 측정한 것은 아니므로 현 설계를 반드시 교체해야 한다는 결론은 내리지 않는다.
