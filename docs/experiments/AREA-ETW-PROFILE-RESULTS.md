# Area CPU·힙 추적 결과

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

## 수집 확인

- 분석 자료: `artifacts/motion-profiles-admin-v2`의 CPU.etl, Heap.etl 및 함께 보관된 실행 파일/PDB.
- CPU 대상 PID 13436, 28만 Actor. 힙 대상 PID 10928, 5,000 Actor. 서로 다른 실행이다.
- 두 추적 모두 Lost Buffers / Lost Events = 0. 힙 추적 설정 해제도 로그에서 확인했다.
- 두 실행 모두 overlap=0, never_updated=0이며 종료 후 Task live=0, pending=0이다.
- 프로파일링 자체의 오버헤드가 있으므로 이 실행의 FPS를 기존 일반 성능 비교에 합치지 않는다.

설정상 CPU 기록은 준비 후 10초, 힙 기록은 시작부터 3초다. 다만 ETL 헤더 범위는 각각 43.532초와 18.072초다. 정지·후처리 구간을 포함하므로 설정 시간으로 할당 수를 나눠 초당 처리량을 계산하지 않았다. 아래 수치는 추적 전체 집계이며 정상 상태만 잘라낸 비교는 아니다.

## CPU 호출 경로

`cpu-sampled-stacks.html`에서 대상 PID의 Profile 이벤트만 필터링했다. 총 57,726개 스택 샘플 기준이다. CSwitch/ReadyThread까지 포함한 기존 `cpu-stacks.txt`는 CPU 점유 비율 분석에 사용하지 않았다.

| 함수 | 자체 샘플 비율 | 하위 호출 포함 비율 |
|---|---:|---:|
| Area::update | 21.30% | 33.94% |
| TestObject::update | 8.14% | 9.87% |
| Area::get_partition | 6.49% | 6.50% |
| WheelTimer::expired_tasks | 2.96% | 4.08% |
| AreaExecuter::update_area | 2.44% | 46.30% |
| AreaQueue::push | 1.88% | 6.72% |
| Worker::submit | 1.66% | 3.08% |
| Worker::run_timer | 1.35% | 27.56% |
| AreaExecuter::enqueue_scheduled | 1.33% | 17.73% |

하위 호출 포함 비율은 중첩되므로 합산하면 안 된다. CPU 샘플은 실행 중인 비용을 보여주며 잠금으로 잠든 시간을 직접 나타내지 않는다.

모듈 자체 샘플은 실행 파일 54.24%, ntdll 22.50%, 커널 20.27%다. OS 심벌이 해석되지 않아 ntdll/커널 비용을 특정 잠금·힙·알림 API로 분류하는 것은 **확인 불가**다. 이 결과만으로 커널 전환이나 특정 mutex를 주범으로 단정할 수 없다.

### 코드와 연결한 해석

- `Source/AreaUpdate/Area.cpp`의 `Area::update()`는 Actor 순회, 활성 상태 확인, pending 처리와 전역 atomic `areaRunCount` 증가를 포함한다. 자체 21.30%를 전부 게임 연산으로 볼 수 없다. 특히 전역 카운터의 스레드 간 경합 여부는 카운터 제거 A/B 측정이나 명령어 단위 분석이 필요하다.
- 같은 파일의 `get_partition()`은 좌표별 나머지 계산을 수행한다. 자체 6.49%가 관측되므로 고정 좌표·고정 partitionWidth에서의 계산 결과 보관은 독립 검증할 가치가 있다. 설정 변경 시 캐시 무효화가 필요하다.
- `AreaExecuter::update_area()`는 매 실행 후 `new PartitionTickTask(...)`를 생성해 다음 절대 시각을 예약한다. 예약·만료·재제출 경로가 비용을 만들고 있다는 것은 CPU와 힙 양쪽에서 확인된다. 단, 전체 비용 중 할당만의 CPU 비중은 확인되지 않았다.

## 힙

힙 추적 전체에서 할당 1,901,643건, 누적 요청 202,249.3KB, 추적 종료 시 outstanding 35,363건 / 42,465.7KB다. outstanding은 프로세스 실행 중 살아 있던 할당이며 누수 판정이 아니다. 아래 KB는 도구 출력 기준이다.

| 호출 경로 | 누적 할당 | 종료 시 outstanding | 해석 |
|---|---:|---:|---|
| AreaManager::build | 2건 / 36,800.1KB | 36,800.1KB | 맵 저장 공간 |
| main의 Actor 생성 경로 | 5,000건 / 4,960.9KB | 4,960.9KB | Actor 저장 공간 |
| AreaExecuter::update_area | 1,606,688건 / 100,418.0KB | 4,884건 / 305.3KB | 다음 tick Task 예약 |
| Area::apply_membership_changes 아래 vector 삽입 | 23,625건 / 193.6KB | 184.9KB | Actor 목록 용량 |

맵과 Actor가 outstanding 용량의 약 98.3%다. 예약 경로는 많은 할당을 만들지만 살아 있는 양은 상대적으로 작다. 이번 자료는 장시간 메모리 누수나 프로세스 전체 메모리 증가를 입증하지 않는다. 종료 로그의 Task live=0도 Task에 한정된 검증이다.

vector 삽입 스택은 도구에서 `vector<Task*>` 심벌로 표시되지만 실제 호출 지점은 Actor 목록 삽입이다. 최적화된 동일 코드의 심벌 표시만으로 Area가 Task 목록을 소유한다고 해석하지 않았다.

## 다음 개선 우선순위

1. 디버깅용 전역 `areaRunCount`를 끈 A/B 측정. 자체 Area 비용 중 계측 경합이 차지하는 영향을 먼저 분리한다. 현재 추적만으로 효과 크기는 확인 불가다.
2. partition 인덱스의 반복 나머지 계산 제거를 독립 비교. 반경·공평성·분할 규칙은 유지한다.
3. Area 재예약 시 Task를 매번 생성하는 경로를 줄이는 설계 검토. 오브젝트 풀 대신 Area 수명에 연결된 예약 항목을 검토하되 취소·중복 예약·이동·종료 소유권을 먼저 검증해야 한다.

이번 작업에서는 실행 로직을 변경하지 않았다. 잠금 대기 주범의 확정과 개선 효과의 크기는 위 변경을 각각 적용한 재측정이 필요하다. 실제 게임 로직 자료가 없으므로 게임 서버 전체 성능의 결론으로 확장하지 않는다.

후속으로 세 항목을 순차 비교했다. 카운터 결과는 [1단계 보고서](AREA-COUNTER-COMPARISON.md), 캐싱·예약 묶음 및 계측 OFF 확인은 [후속 보고서](AREA-FOLLOWUP-COMPARISON.md)에 정리했다.
