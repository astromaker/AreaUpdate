# 동작 유지 리팩토링

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

## 변경 사항

1. `AreaManager`: allocator 기반 연속 RAII 저장소. Area 이동·복사 없이 정렬과 주소 안정성을 유지한다. 복사는 금지하며, 새 맵을 완성한 뒤 교체해 생성 실패 시 기존 맵을 보존한다. 차원과 할당 크기 검증도 추가했다.
2. `Area`: pending 요청 회수, 목록 변경, 소유권 이전을 각각 분리했다. Actor 잠금 → pending 잠금 순서와 잠금 밖 wake callback 호출을 유지한다. Actor 실행 순서는 바꾸지 않았다.
3. `AreaExecuter`: partition 진입, batch 처리, 종료 및 회전을 분리했다. queue 소유권은 unique_ptr로 관리한다. `m_partitionAdmissionAttempts`는 빈 pop도 포함한 기존 진입 시도 횟수다.
4. `AreaQueue`: deque의 front/pop_front로 FIFO 추출을 표현한다. count 0이 전체 추출이라는 규칙을 유지한다.
5. `Worker`와 `JobDispatcher`: `WindowsHandle`로 event/timer 수명을 관리한다. Worker의 준비 작업 검사도 한 함수로 통일하되 waiting 공개 후 재검사하는 순서를 유지한다.
6. 비교 프로그램: `ComparisonLayout.h`에서 seed 기반 입력을 생성하고 `ComparisonTiming.h`에서 시간과 주기를 측정한다. World에는 방식별 tick 함수와 별도 상태 검증 함수가 있다. hot path에 새 가상 인터페이스나 std::function 계층을 추가하지 않았다.
7. 현재 코드의 API는 snake_case로 통일하고 들여쓰기·CRLF를 정리했다. Git 원본 스냅샷과 원본 어댑터의 API는 변경하지 않았다.

## 유지한 정책

- 반경 2, 색상 간격 5, 색상 수 25, batch 64.
- partition 진입 시도 상한 8, Area 재예약 간격 50ms, Worker 요청 처리 상한 256.
- 한 partition만 실행, Active 필터, Actor 소유권과 중복 예약 방지.
- Job의 round-robin 재전달, TLS 정책, 일반 new/delete. 오브젝트 풀과 idle stealing은 추가하지 않았다.
- 비교 CLI 인수와 결과 출력 필드, tick 종료 barrier, damage/checksum 검증.

## API 변경 예

| 이전 | 현재 |
|---|---|
| addObject / removeObject / moveObject | add_object / remove_object / move_object |
| getPartition / isActive | get_partition / is_active |
| setActive / setWake | set_active / set_wake |
| requestSchedule / finishSchedule | request_schedule / finish_schedule |
| getX / getY | get_x / get_y |
| getCountX / getCountY | get_count_x / get_count_y |

저장소 안의 호출부를 함께 변경했다. 외부 프로젝트가 기존 이름을 호출한다면 갱신해야 한다. 불필요한 중복 별칭은 남기지 않았다.

## 수명 전제

`AreaManager::build()`와 재빌드는 Worker를 중단하고 기존 Area 포인터 사용을 끝낸 상태에서 호출해야 한다. 성공한 재빌드는 기존 포인터를 무효화한다. RAII 도입이 실행 중인 월드의 자동 교체나 Actor 수명 관리까지 제공하는 것은 아니다.

## 추가한 회귀 시험

- AreaManager의 생성 중 예외 정리, 실패한 재빌드 후 이전 맵 보존, 성공한 재빌드 시 이전 맵 해제.
- 과도 정렬 타입의 연속 배치, 복사 금지, 잘못된 차원 거부.
- AreaQueue의 FIFO와 제한/전체 추출.
- Windows handle 이동, 유효 handle 동작, 종료 후 handle 수 복원.

기존 음수 좌표, Actor 이동/중복 등록, partition 배타성/공평성, 타이머, 종료 중 제출 테스트도 유지한다. 리팩토링은 성능 향상을 주장하는 변경이 아니며, 기존 측정 보고서의 수치를 새 빌드 성능으로 재사용하지 않는다.

## 검증 결과

- MSBuild 솔루션 x64 Release/Debug 빌드 성공, 두 구성의 AreaTests와 UpdateComparison self-test 통과.
- PowerShell 집계 회귀 테스트 통과.
- 보관한 기존 측정 바이너리와 새 빌드에 중앙 집중·한 Area 집중·640×640 큰 맵·주기 실행·연산 20,000회 조건을 입력했다. 총 5조건 × 4모드 × 전후 2개 = 40개 결과에서 checksum·이벤트 수·동시 접근 위반 값이 대응 일치했다. 로그는 `artifacts/refactoring/*-before.log`, `*-after.log`다.
- 실제 실행기 30.211초, Actor 10,000개 이동 시험: 이동 48,876회, 업데이트 5,651,184회, 평균 18.706 FPS. 중복 업데이트 0, 미실행 Actor 0, 종료 후 pending/live Task 0. 생성/파괴 Task는 각각 5,393,510개였다.
- Git 원본 7개 파일은 저장소 줄바꿈 정규화 후 blob hash 일치. 새 코드와 비교용 원본을 혼합 수정하지 않았다.

위 전후 실행은 상태 보존 검증이며 통제된 성능 A/B 시험은 아니다. 기존 63회 성능 비교를 이번 리팩토링에서 다시 수행한 것은 아니다.
