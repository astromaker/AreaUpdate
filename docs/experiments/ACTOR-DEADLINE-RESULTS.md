# 이동 중 Actor 갱신 시각 유지

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

## 변경

- Actor가 다음 갱신 시각을 보유한다. Area 이동 시 이 값을 초기화하지 않는다. 완전 탈퇴 후 새로 등록할 때만 초기화한다.
- 갱신 시각 전에는 Actor callback을 실행하지 않아 목적지의 조기 tick에 의한 중복/과도한 갱신을 막는다.
- 지연된 경우 마지막 목표 시각을 기준으로 다음 50ms 주기를 계산한다. 누락된 주기를 몰아서 여러 번 실행하지 않는다.
- 이동 예정 Actor가 이미 갱신 시각을 지났다면 소유권을 넘기기 전에 출발 Area에서 갱신한다. callback 동안 출발 Area의 공간 실행 보호를 유지하며, 목적지는 이 callback 종료 전 Actor를 실행할 수 없다.
- 출발 Area에서 이탈 목록을 반영한 후 `do_update()`로 실행 문맥을 준비하고, 잔류 Actor와 갱신 시각이 지난 이동 Actor를 처리한 뒤 목적지에 소유권을 게시한다. 완전 탈퇴 요청은 마지막 callback을 추가 실행하지 않는다.
- Area 예약은 잔류 Actor의 가장 이른 갱신 시각도 반영한다. 이는 새 partition AreaExecuter와 최적화된 radius Runtime 양쪽에 적용한다. 기존 타이머 예약 중복을 추가하지 않는다.
- 이동 횟수 제한, 목적지 첫 갱신 전 재이동 보류, Actor 전용 타이머/Task는 추가하지 않았다. radius 잠금, partition 보호, batch 64 정책은 유지했다.

## 결정적 검사

`Area::update_at()`으로 단조 증가하는 가상 밀리초를 전달한다. 첫 갱신 후 10ms마다 다음 이동을 예약하고 출발 Area를 처리하는 과정을 100번 반복한다.

```text
HANDOFF_CHAIN transfers=100 moving_updates=20 anchor_updates=39 resumed_updates=1 detached=1
```

1초간 이동 100번을 모두 허용하면서 Actor는 정확히 20번 갱신한다. 마지막 출발 Area 갱신과 동일한 시각의 목적지 tick에는 추가 갱신하지 않으며, 다음 50ms에 1번 실행한다. 이 검사는 이전의 '이동 중 0번 갱신' 재현을 시간/중복 보장 회귀 검사로 바꾼 것이다.

추가 단위 검사: 목적지의 이른 tick 건너뛰기, 같은 시각 반복 호출 중복 방지, 이른 Actor deadline의 Area 예약 반영, 160ms 이상 지연 후 catch-up 폭주 방지, 이동 전 위상 유지, 완전 탈퇴 후 callback 없음.

직접 tick을 연속 실행하는 비교 벤치마크는 현실 시간이 흐르지 않으므로 tick마다 명시적으로 50ms 가상 시간을 전달하도록 수정했다. 실제 Worker 기반 실행은 계속 steady_clock을 사용한다.

## 한계

이 변경은 이동 때문에 Actor 갱신이 계속 사라지는 경로를 제거한다. 운영체제 지연, 큐/radius 대기, 긴 callback 때문에 50ms 목표를 넘기는 것까지 막는 실시간 보장은 아니다. 이미 대기 중인 목적지 타이머를 강제로 취소/앞당기지 않으므로 출발/목적지 실행 시점에 따른 일부 인계 대기는 남을 수 있다.

정량 결과와 로그: `artifacts/actor-deadline-fix/`.

## 수정 전후 실측

같은 머신에서 수정 전 보관 실행 파일과 수정 후 Release 실행 파일을 번갈아 실행했다. 각 15초, Actor 5,000개, `verify` 합성 이동/상호작용, 이동 시도 64, settled 제한 없음, 상세 추적 비활성이다. FPS는 정상 구간 측정값이다.

| 모델 / 배치 / Seed | FPS 전 → 후 | 최대 간격 전 → 후 | p99 구간 상한 전 → 후 | 프로세스 CPU 초 전 → 후 |
|---|---:|---:|---:|---:|
| radius / uniform / 42 | 19.595 → 19.999 | 186 → 69ms | 90 → 70ms | 4.438 → 3.625 |
| radius / uniform / 43 | 19.597 → 20.019 | 166 → 83ms | 90 → 70ms | 5.203 → 4.063 |
| radius / boundary / 42 | 19.655 → 20.003 | 201 → 69ms | 100 → 70ms | 0.953 → 1.578 |
| partition / uniform / 42 | 19.606 → 19.983 | 165 → 102ms | 90 → 70ms | 1.750 → 2.188 |

구간 경계와 측정 시간 때문에 FPS는 20보다 약간 높게 표기될 수 있다. CPU 시간은 전체 실행의 누적 프로세스 CPU 초이며 CPU 사용률 퍼센트가 아니다. CPU 비용은 배치에 따라 감소하거나 증가했다. 특히 boundary와 partition에서 지연 개선에 CPU 증가가 동반됐다. 실행 시점에 따라 상호작용/이동 성공 수가 달라지므로 checksum 동일성을 성능 비교 조건으로 사용하지 않았다.

8회 모두 이벤트 전송/적용 개수 일치, overlap 0, 소유권 왕복 검증 성공, 종료 시 live Task 0이었다. 이 결과는 합성 검사의 관측값이며 실제 게임 로직 성능이나 최악 지연 상한을 뜻하지 않는다.

### 70,000 Actor Worker 실행 점검

각 10초, map 640, 초당 이동 시도 8,000, 입장 1,000ms/1ms 간격, signal 대기, Task 통계 활성, seed 42이다. 이 검사는 수정 후의 실행 건전성 점검으로, 수정 전후 성능 비교와 구분한다.

| 모델 | 정상 구간 FPS | 정상 구간 CPU 사용률 | 최대 간격 | 미갱신 Actor | 종료 live Task |
|---|---:|---:|---:|---:|---:|
| radius | 19.982 | 36.89% | 107ms | 0 | 0 |
| partition | 19.952 | 34.06% | 105ms | 0 | 0 |

CPU 사용률은 머신 전체 논리 CPU 대비 비율이다. 두 프로세스 모두 종료 코드 0, Task 생성/파괴 균형을 확인했다. 최대 간격 100ms 이상이 여전히 관측되므로 지연이 완전히 제거됐다고 해석하면 안 된다.

## 검증

- `Source/AreaUpdate.sln` MSBuild Release/Debug x64 빌드 성공.
- 두 구성 `AreaTests`, 비교 `--test`, `--handoff-chain-probe` 통과.
- `tools/quality/Format-Code.ps1 -All -Check`, `tools/quality/Check-CodeStyle.ps1`, `git diff --check` 통과.
- 기존 C++ BOM을 보존하고 변경 파일은 CRLF로 정리했다.
- Core, ObjectUpdate 실행 정책, frozen Original/Baseline은 수정하지 않았다.
