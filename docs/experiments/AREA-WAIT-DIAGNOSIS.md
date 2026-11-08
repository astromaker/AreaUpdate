# Area CPU 활용 부족: admission barrier와 폴링 대기

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

이 문서는 기본값 변경 전의 진단 기록이다. 후속 적용으로 현재 기본값은 signal이다.
signal을 기본으로 맞춘 두 모델 비교는 SIGNAL-MODEL-COMPARISON.md에 별도로 정리했다.

## 확인된 실행 경로

AreaExecuter::try_enter_partition은 현재 partition의 누적 진입 시도를 최대 8회 허용한다.
이는 동시에 실행하는 Worker 수 제한만이 아니다. 같은 Worker도 여러 번 소비할 수 있다.
진입 시도는 try_pop보다 먼저 증가하며, 큐 lock 실패/빈 pop도 시도 횟수에 포함된다.

8회를 소비하면 has_work는 대기 중인 Area가 남아 있어도 false를 반환한다.
leave_partition은 현재 runner가 모두 끝나야 다음 partition으로 넘어가며,
그때 admission 횟수를 초기화하고 wake_workers를 호출한다.
공간 충돌을 피하려고 서로 다른 partition의 동시 실행은 허용하지 않는다.

Worker는 Ticker에서 실행 가능한 일이 없다고 판단하면 wait_for_work로 간다.
poll에서는 sleep_for(1ms)를 사용한다. wake는 대기 상태 표시를 바꾸지만 sleep을 중단하지 못한다.
따라서 partition이 전환돼 실행 가능해진 이후에도 잠든 Worker의 복귀를 기다릴 수 있다.

```text
누적 진입 8회 소진
→ 남은 Area가 있어도 추가 진입 금지
→ 먼저 끝난 Worker들은 마지막 runner를 기다리며 대기
→ 마지막 runner가 partition 전환·깨우기 요청
→ poll의 sleep은 중단되지 않음
→ 실제 복귀 후에야 다시 실행
```

이는 특정 mutex가 오랫동안 잡혀 있다는 추측과 다르다.
실행을 허용하지 않는 admission 조건과 이를 기다리는 대기 경로가 코드에 명시돼 있다.
하지만 빈 pop과 lock 실패 중 어느 쪽이 얼마나 자주 발생하는지는 현재 통계로 구분하지 못한다.

## 기존 로그의 근거

직전 고정 주기 비교의 14만 Area 첫 실행은 다음과 같다.

- 11.758FPS, CPU 16.80%.
- wait 11,640회 중 blocked_samples 11,310회, 약 97.2%.
- 여기서 blocked는 readyWork가 남아 있으나 admission 예산 때문에 runnable=false인 시점의 표본이다.
- Worker 합계 대기 134,534.7ms, busy 25,501.5ms. 기록 구간 합계의 약 84.1%가 대기였다.
- 실제 poll 대기 구간 평균은 약 11.56ms였다. 요청한 1ms와 같지 않았다.

이는 벽시계 측정이며 OS의 정확한 타이머 해상도나 순수 CPU 시간을 뜻하지 않는다.
소수의 busy/ready 경쟁 시점도 있으므로 blocked 횟수 비율을 전체 시간 비율로 해석하지 않는다.

## 기존 signal 옵션으로 대조

프로덕션 코드는 수정하지 않았다. 같은 고정 주기 실행 파일에서 WaitMode만 변경했다.
고정 50ms, 5ms·256슬롯, Task 통계 ON, 추가 프로파일 OFF, Actor 추가 연산 0이다.
각 조건 20초씩 2회, poll→signal / signal→poll 순서로 실행했다.
14만은 맵 453×453·이동 시도 4,000회/초,
28만은 640×640·8,000회/초다. 모두 5초 분산 입장·1ms 입장 확인·seed 42다.
CPU는 전체 논리 CPU 8개 기준이고 입장 완료 후 1초 이후의 시간 가중 평균이다.

| Actor / 모드 | FPS 1회 / 2회 | CPU 1회 / 2회 |
|---|---|---|
| 14만 poll | 11.141 / 11.845 | 16.61 / 16.93% |
| 14만 signal | 19.999 / 20.008 | 51.38 / 53.45% |
| 28만 poll | 5.810 / 5.788 | 17.19 / 17.32% |
| 28만 signal | 11.898 / 11.966 | 60.25 / 63.23% |

signal은 조건 변수 알림으로 대기를 해제하며 다음 타이머까지 제한 대기한다.
이 옵션은 더 이른 요청에 대한 깨우기 및 미래 타이머 등록의 알림 병합도 포함한다.
따라서 이 실험은 대기 경로의 조합을 대조한 것이지 sleep 호출 한 줄만의 비용 분리는 아니다.
Area의 다음 타이머는 해당 Area를 처리한 Worker에 제출한다.

14만 signal은 목표 20FPS 근처로 회복됐다.
28만은 처리량이 약 두 배로 늘었지만 여전히 20FPS 미달이며 CPU도 약 60~63%다.
따라서 이 조건에서 깨울 수 없는 poll 대기가 큰 제한이었던 것은 확인되지만,
그것만이 모든 규모의 유일한 병목이라고 할 수는 없다.

### 14만의 대기 길이

| 모드 / 회차 | wait 횟수 | 대기 벽시계 평균 | 평균 pop Area / admission |
|---|---:|---:|---:|
| poll / 1 | 11,591 | 11.59ms | 61.52 |
| poll / 2 | 11,772 | 11.41ms | 60.89 |
| signal / 1 | 584,753 | 약 0.125ms | 38.03 |
| signal / 2 | 584,438 | 약 0.125ms | 38.00 |

signal은 대기 횟수 자체를 줄이지 않았다. 짧게 기다리고 실행을 재개하는 횟수가 증가했다.
blocked 횟수 비율도 signal에서 약 99.6%지만 대기 구간이 훨씬 짧아 FPS는 높았다.
따라서 blocked 비율 하나만으로 속도를 판단하면 안 된다.
pop당 Area 수가 줄었다는 관측만으로 lock 실패와 정상적인 빈 partition 탐색을 구분할 수는 없다.

## 결론과 다음 확인 대상

현재의 큰 원인은 admission/partition barrier와 깨울 수 없는 poll 대기가 결합된 것이다.
CPU를 사용할 일이 없는 게 아니라, 실행 가능 시점 전환을 늦게 따라가고 있었다.
앞선 Object에서 signal 이득이 작았다는 결과를 Area에 그대로 적용하면 안 된다.
Object는 같은 Area partition admission barrier를 사용하지 않는다.

우선 적용 후보는 Area Worker에만 기존 signal 대기를 선택하는 것이다.
반경·partition 간 배제·batch 64·공평성을 무시하고 has_work를 항상 true로 만들거나
sleep만 제거하면 CPU를 쓰는 busy-spin일 뿐 처리량 개선이 보장되지 않는다.

28만의 남은 제한을 줄이려면 성공/실패/빈 pop을 구분한 admission 통계,
phase를 닫은 뒤 마지막 runner를 기다린 시간, 성공 batch 수 대비 시도 수를 먼저 확인해야 한다.
그 결과를 보고 동시 실행 수와 turn 작업 예산을 분리할지 판단해야 한다.
8회 제한을 무조건 제거하면 partition 공평성이 바뀔 수 있어 이번 진단에서 변경하지 않았다.
전역 mutex/allocator/atomic의 CPU 비용은 별도 CPU 프로파일 없이는 확정할 수 없다.

이번 작업에서는 기본 poll 설정과 소스 코드를 유지했다.
실제 게임의 무거운 연산·상호작용 부하에는 결과를 그대로 일반화하지 않는다.

## 자료와 재현

artifacts/area-wait-diagnosis에 실행 파일·핵심 소스·원본 로그·CPU 표본·summary.json·results.csv를 보존했다.
admission-140000.csv는 대기 길이와 pop 통계 분석이다.
실행 파일 SHA-256은 77126D4A25E01E61D9ADDC7525E95A823F7FA52D719E25E75AB7AE1A80935265다.
8회 모두 종료 코드 0, 중복·미갱신 Actor·잔여 Task 0을 확인했다.

```powershell
# 현재 소스에서도 사용할 수 있는 기존 옵션. 기본값은 아직 poll.
.\Source\x64\Release\AreaUpdateExample.exe 20 140000 4000 42 5000 1 0 453 area 0 signal
```
