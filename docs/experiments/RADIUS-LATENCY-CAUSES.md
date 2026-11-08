# 남은 Actor 갱신 지연 원인

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

후속 수정: 이동 중 Actor deadline 유지와 인계 전 overdue 갱신을 적용했다. 아래 내용은 수정 전 진단 기록이며, 현재 동작과 결과는 `ACTOR-DEADLINE-RESULTS.md`를 참고한다.

## 결론

이동 중 목적지 Actor 갱신 전에 다음 이동을 허용하는 경로가 확인됐다. 이는 radius 잠금만의 문제가 아니라 두 Area 모델이 사용하는 공통 Actor 인계 경로의 문제다. 타이머 지연과 고부하 큐 대기도 별도로 존재한다. 이번 변경은 기본 비활성 진단과 재현 검사이며, 운영 인계 정책은 변경하지 않았다.

## 1. 연속 인계로 Actor 갱신 건너뛰기

`Source/AreaUpdate/Area.cpp`의 `complete_transfers()`는 목적지 pending-add에 Actor를 넣고 소유자를 변경하면서 `m_leaving`을 해제한다. 목적지에서 아직 Actor를 갱신하지 않았어도 다음 `move_object()`가 성공한다. 목적지 `update()`는 Actor 갱신보다 먼저 추가/제거를 처리하므로 같은 Actor를 추가한 뒤 바로 제거할 수 있다.

따라서 A → B → C → D 이동 동안 B와 C의 Area tick은 실행돼도 해당 Actor의 update는 실행되지 않을 수 있다. 이미 예약된 목적지의 실행 시점은 새 인계 때문에 앞당겨지지 않는다.

단일 스레드 재현 명령:

```powershell
.\Source\x64\Release\UpdateComparison.exe --handoff-chain-probe
```

실측 출력:

```text
HANDOFF_CHAIN transfers=100 moving_updates=0 anchor_updates=100 resumed_updates=1 detached=1
```

타이머, Worker, radius 잠금 없이도 100번 인계 동안 이동 Actor 갱신은 0번이다. 이동을 멈추면 다음 갱신이 실행된다. 실제 이동 추적에서도 198.118ms 갱신 간격에 연속 이동 4번이 기록됐다.

### 비계측 대조군

legacy, Actor 5,000개, uniform, 각 20초. settled는 목적지에서 Actor가 갱신될 때까지 다음 이동을 막는 진단 전용 조건이다.

| Seed | 조건 | 정상 구간 FPS | 최대 간격 |
|---|---|---:|---:|
| 42 | 이동 없음 | 19.959 | 87ms |
| 42 | 기존 이동 | 19.621 | 185ms |
| 42 | settled 이동 | 19.639 | 123ms |
| 43 | 이동 없음 | 19.997 | 69ms |
| 43 | 기존 이동 | 19.638 | 199ms |
| 43 | settled 이동 | 19.669 | 134ms |

settled는 이동 성공 수도 달라지므로 동등 성능 개선 결과가 아니다. 원인 분리를 위한 검사다. 새 Area에서도 이동 없음 최대 70ms, 기존 이동 최대 184ms였다.

## 2. 타이머 지연의 내부 구간

Actor/Area 상세 추적을 끄고 기존 Core 타이머 추적만 켠 대조군에서 다음 47ms 지연을 확인했다.

| 구간 | 지연 |
|---|---:|
| 목표 deadline → 5ms 휠 실행 가능 시점 | 4ms |
| 실행 가능 시점 → timed wait 반환 | 22ms |
| 반환 후 poll → 해당 callback 실행 | 21ms |

마지막 구간은 앞선 타이머 callback 처리 등을 포함한다. 이 callback은 Actor 계산이 아니라 Area 큐 게시/알림 경로다. timed wait 내부의 OS 스케줄링과 타이머 정밀도 기여, callback 내부 잠금과 선점의 기여는 현재 추적만으로 분리 확인 불가다. 단순히 휠 해상도나 sleep만의 문제로 볼 수 없다.

## 3. 고부하에서는 Area 실행 대기가 별도 병목

280,000 Actor, map 640, seed 42, 각 20초, Task 통계 활성 상태의 저빈도 표본 추적 결과:

| 조건 | 정상 구간 FPS | 최대 Actor 간격 | 표본 평균 타이머 지연 | 표본 평균 Area 실행 대기 |
|---|---:|---:|---:|---:|
| 이동 없음, 표본 추적 | 17.016 | 110ms | 1.975ms | 32.494ms |
| 초당 이동 시도 8,000, 표본 추적 | 16.749 | 216ms | 2.103ms | 31.942ms |

Area 실행 대기는 큐 게시 직전부터 update 시작까지이며 큐 잠금, radius 충돌, batch 내 앞선 작업 대기를 합친 값이다. 각각의 비율은 확인 불가다. 최초 활성화도 포함한 전체 실행 표본이며 정상 구간만의 통계가 아니다.

표본 추적 없는 이동 없음 대조군도 최대 147ms였다. 계측에 따른 시간 변동이 있으므로 위 값을 운영 성능의 엄밀한 한계로 해석하면 안 된다. 다만 이동이 없어도 고부하에서는 지연이 남으며 타이머 만료 이후 대기가 크다는 방향은 확인했다.

## 수정 방향

1. 목적지 인계 완료/최초 Actor 갱신 전에 다음 이동을 허용할지 정책부터 정한다. 단순 재이동 제한은 이동 의미를 바꾸므로 바로 적용하지 않았다.
2. 이동해도 Actor의 다음 목표 갱신 시각을 보존하고 목적지 예약과 연결하는 방식을 검토한다. 중복 갱신과 소유권 충돌 방지가 선행돼야 한다.
3. 고부하 큐 대기는 radius admission과 batch 내부 대기를 나눠 추가 계측해야 한다. 타이머만 바꿔서는 해결되지 않는다.

## 근거 파일

- `artifacts/radius-latency/controls/results.csv`: 비계측 이동 대조군
- `artifacts/radius-latency-v2/trace/`: Actor 인계 기록과 Core 전용 타이머 기록
- `artifacts/radius-latency-v3/capacity/`: 저빈도 표본 고부하 대조군
- `artifacts/radius-latency-v3/handoff-proof.log`: 결정적 단일 스레드 재현

진단은 기본 비활성이며 기존 실행 옵션의 기본값은 유지했다. 상세 추적도 실행 시간을 바꾸므로 비계측 대조군과 구분했다.

검증: Release/Debug 솔루션 빌드, 두 구성 AreaTests, Release 비교 회귀 검사와 두 구성 단일 스레드 인계 재현을 확인했다. 전체 포맷 검사, 코드 스타일 검사, 스크립트 구문 검사도 통과했다. 기준으로 보관한 AreaUpdate/Core 파일 SHA256은 변경되지 않았다.
