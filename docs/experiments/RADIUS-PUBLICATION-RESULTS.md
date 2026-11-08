# 예전 Area 묶음 게시 최적화

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

이후 동일 bucket 및 알림 통합의 후속 결과와 최신 기본값은 [RADIUS-FAST-RESULTS.md](RADIUS-FAST-RESULTS.md)에 정리했다.

## 변경 범위

예전 Area 최적화 경로인 legacy의 큐 게시를 개선했다. 새 Area, 원본 Baseline, Worker/타이머 휠, Actor 소유권은 변경하지 않는다. radius 2의 5×5 잠금, 추출 batch 64, 큐 선두 충돌 시 해당 큐 중단은 유지한다. 오브젝트 풀, idle Worker 관리, stealing은 추가하지 않는다.

## 구현

RadiusExecuter::push_batch는 최대 64개 Area/bucket 쌍을 받는다. bucket을 정규화하고 원래 순번을 보조 키로 정렬해 같은 bucket 안의 입력 순서를 유지한다. bucket별로 한 번씩 큐 mutex를 잡고 모두 삽입한다. 여러 producer 사이의 전체 순서는 정의하지 않지만, 각 큐에 게시된 순서는 기존 FIFO로 소비한다.

m_ready는 첫 큐를 공개하기 전에 전체 개수를 한 번에 예약한다. 다른 Worker가 일부 큐를 즉시 소비해도 카운터가 음수로 내려가지 않는다. 모든 큐 공개가 끝난 뒤 generation을 증가시키고 한 번만 wake_workers를 호출한다. 마지막 Runner가 중간에 잠들더라도 최종 게시 알림이 남도록 완료 후 알림은 생략하지 않는다.

삽입 중 예외가 나면 미게시 예약 개수만 카운터에서 제외하고 이미 게시한 작업에 대해 알린 뒤 예외를 전달한다. 원자적인 all-or-nothing 제출이나 Worker callback 예외 복구를 보장하는 API는 아니다. deque 할당 실패 주입은 별도로 검증하지 않았다.

RadiusTimerBatchTask가 이 경로를 사용한다. 서로 다른 절대 만료 시각을 합치지 않는다. 단일 예약 Task는 기존 push 경로를 사용한다. 따라서 timer 묶음과 publication 묶음은 별도 옵션이며, publication 옵션만 켜고 timer 묶음을 끄면 이 경로가 사용되지 않는다.

## 안전성 검사

테스트 전용 훅으로 다음 순서를 강제로 만든다.

- Worker가 대기 상태를 공개한 직후 게시하고, 실제 signal 대기에 들어갈 때까지 최종 알림을 보류한다. 이후 알림으로 모든 작업이 실행되는지 확인한다.
- 첫 bucket만 게시한 상태에서 생산자를 멈춘다. Worker가 그 작업을 끝내고 미게시 예약만 남은 상태로 잠든 뒤 나머지 bucket을 공개해 최종 알림으로 재개하는지 확인한다.
- 같은 bucket 안의 입력 순서, bucket modulo, 빈 제출, 잘못된 크기/null 입력, 준비 단계 오류 시 ready 개수 보존을 검사한다.
- 두 생산자와 네 Worker가 64개 Area를 50회씩, 총 3,200회 처리해 누락/잔여 개수가 없는지 검사한다.

훅은 테스트 빌드에만 존재한다. 성능 실행 파일에는 포함되지 않는다. 타임아웃 watchdog로 실패를 관찰하며 Worker stop의 알림을 성공으로 오인하지 않도록 stop 전에 완료 여부를 기록한다.

## 측정 설정

MSBuild Release x64, Task 통계 OFF, 잠금 계측 OFF, Worker 8개, signal 대기, Actor 추가 연산 0, 고정 50ms 예약, 1초 동안 1ms 간격 입장이다. seed 42/43에서 실행 순서를 반전한다. 각 모드가 같은 실행 파일과 공통 payload를 사용한다. 실게임 로직은 제공되지 않아 합성 부하로만 비교한다.

Run-RadiusPublication.ps1은 area(새 Area), single(예전 기본 설정), timer(예약만 묶음), publish(예약+게시 묶음)를 분리한다. scout는 각 15초, 후속 final/boundary는 각 20초, profile은 별도 10초다. 원자료와 고정 실행 파일/PDB는 artifacts/radius-publication 아래에 보관한다.

### 28만 사전 비교

| 설정 | FPS | CPU | 두 실행 중 최대 갱신 간격 |
|---|---:|---:|---:|
| 새 Area | 19.982 | 82.36% | 196ms |
| 예전 개별 예약 | 12.104 | 87.14% | 351ms |
| 타이머만 묶음 | 12.021 | 88.10% | 245ms |
| 타이머+게시 묶음 | 16.292 | 85.91% | 205ms |

### 게시 비용 진단

별도 계측 실행에서 timer/publish를 비교했다. 계측 자체의 비용이 있어 위 성능 실행과 합산하거나 CPU 비중으로 환산하지 않는다.

| 계측 항목 | 타이머만 묶음 | 타이머+게시 묶음 |
|---|---:|---:|
| 게시 Area 수 | 20,237,231 | 26,884,854 |
| 게시 호출 수 | 20,237,231 | 5,743,571 |
| 큐 잠금 횟수 | 20,237,231 | 5,743,571 |
| wake_workers 호출 | 6,604,799 | 5,735,610 |

게시 Area당 큐 잠금이 1회에서 약 0.214회로 감소했다. wake 호출도 처리 Area당 감소했다. 이 실행의 timer 묶음은 같은 bucket에서 추출한 Area들이므로 게시 호출당 큐 잠금이 한 번이었다. 일반 push_batch API는 여러 bucket을 포함하면 bucket 수만큼 잠근다.

예약 Task 수도 처리 Area당 크게 줄었다. 게시 묶음이 실행/예약 흐름에 영향을 주는 결과이지만, 개별 카운터·큐 잠금·알림의 기여율을 독립적으로 분리한 실험은 아니다. m_ready 캐시 경합의 하드웨어 비중은 확인 불가다.

## 후속 교차 비교

각 20초, seed 42/43 평균이다. CPU는 전체 논리 프로세서 대비다.

| Actor | 새 Area FPS / CPU | 예전 개별 예약 FPS / CPU | 예전 묶음 게시 FPS / CPU |
|---|---:|---:|---:|
| 7만 | 19.993 / 23.89% | 19.990 / 39.07% | 19.965 / 34.77% |
| 14만 | 20.001 / 42.56% | 19.943 / 66.47% | 19.997 / 61.26% |
| 28만 | 19.625 / 81.51% | 11.707 / 87.27% | 15.804 / 85.61% |

28만 FPS는 약 35.0% 증가했다. 7만/14만에서는 약 20 FPS를 유지하면서 CPU가 각각 약 4.30%p/5.22%p 낮아졌다. 7만 FPS의 작은 차이와 두 seed만으로 일반적인 무회귀를 보장하지는 않는다.

| 28만 갱신 편차 | 개별 예약 | 묶음 게시 |
|---|---:|---:|
| 두 실행 중 최저 Actor FPS | 11.209 | 14.944 |
| 두 실행 중 최대 갱신 간격 | 251ms | 205ms |

새 Area는 같은 비교에서 최저 Actor FPS 19.031, 최대 갱신 간격 261ms였다. 최대 지연 한 건만으로 전체 모델 우열을 결정할 수 없다. 새 Area는 평균 처리량과 CPU 효율에서 여전히 우세했다. 최종 성능 비교의 미갱신 Actor는 모두 0이었다.

프로세스 peak private memory 평균은 28만 개별 예약 532.12MiB, 묶음 게시 521.92MiB였다. 직접 계측한 할당 byte 합계가 아니므로 Task 개수 감소율과 동일한 메모리 절약률로 해석하지 않는다.

## 기본값과 재현

legacy에서는 타이머 묶음과 게시 묶음을 기본 ON으로 채택한다. batch 64와 진단 계측 OFF는 그대로다. 새 Area 설정은 바뀌지 않는다.

- AREAUPDATE_RADIUS_TIMER_BATCH=0, AREAUPDATE_RADIUS_PUBLISH_BATCH=0: 이전 개별 예약/개별 게시 설정.
- AREAUPDATE_RADIUS_TIMER_BATCH=1, AREAUPDATE_RADIUS_PUBLISH_BATCH=0: 타이머 묶음만 비교.
- 두 변수 모두 1 또는 미설정: 새 기본 설정.

예전 Run-RadiusFollowup.ps1에는 publication OFF를 명시해 종전의 타이머 단독 비교 의미를 보존했다. Test-RadiusFollowup.ps1은 게시 ON/OFF × 타이머 ON/OFF × batch 1/4/16/64 조합을 검사하도록 확장했다.

## 경계 밀집

7만 Actor를 중앙 인접 2×2 Area 안에 두고 이동도 이 범위로 제한했다. 각 20초, seed 42/43 평균이다.

| 설정 | FPS | CPU | 두 실행 중 최대 갱신 간격 |
|---|---:|---:|---:|
| 새 Area | 19.989 | 2.80% | 117ms |
| 예전 개별 예약 | 19.981 | 2.94% | 123ms |
| 예전 묶음 게시 | 19.969 | 3.16% | 132ms |

두 방식 모두 약 20 FPS다. 보호 범위가 겹치는 배치라 실행 batch가 하나씩 추출되고 예약 Task 수는 실행당 1,600개로 동일했다. 이 경우 묶음 게시의 이득은 없었다. 최대 지연도 항상 개선되는 것은 아니다.

## 최종 검증

- MSBuild Source/AreaUpdate.sln Release/Debug x64 빌드 성공. 최종 실행 파일은 기본 Task 통계 ON이다.
- Release/Debug AreaTests 통과. 두 강제 알림 경계, bucket 내 FIFO, 부분 게시 중 마지막 Runner 대기, 복수 생산자 3,200회 처리, 잘못된 입력 검사를 포함한다.
- UpdateComparison --test 통과. 기존 object/original 경로의 회귀 검사도 포함한다.
- 통계 ON, 5천 Actor: 게시 0/1 × 타이머 묶음 0/1 × batch 1/4/16/64, 이동·이벤트·등록/탈퇴 16회 통과.
- 경계 verify/synthetic × area/legacy × seed 42/43, 8회 통과. overlaps=0, sent=applied, tasks_live=0이었다. verify는 ownership_cycle=1이며 synthetic에서는 등록/탈퇴 검사를 하지 않는다.
- 경계 이벤트 ID 손상 주입이 검증기에 의해 거부되는 것도 확인했다.
- Format-Code.ps1 -All -Check, Check-CodeStyle.ps1, git diff --check 통과. C++의 BOM/CRLF를 유지했다.
- reference SHA256과 대조해 새 Area 및 원본 Baseline 파일이 변경되지 않았음을 확인했다.

통계 OFF 실험은 scout 8회, profile 2회, final 18회, boundary 6회다. profile은 계측 오버헤드가 있는 별도 진단으로 성능 평균에 섞지 않았다. 측정 실행 파일의 기본값과 무관하게 각 옵션을 명시했으며, 기본값 변경은 최종 빌드에 반영했다. 실게임/장시간 운영 안정성 전체를 보장하는 결과는 아니다.

재현 스크립트는 Run-RadiusPublication.ps1과 Test-RadiusFollowup.ps1이다. artifacts/radius-publication/validation에 검증 로그를 보관한다.
