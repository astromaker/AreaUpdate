# 최적화 및 3방식 비교

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

## 적용 범위

- 공간 영향 반경 2, 색상 간격 5, 색상 수 25, 실행 batch 64 유지.
- Area: pending 변경이 없으면 목록 mutex 생략. 추가 시 소유권 불변식으로 중복 선형 검색 제거. 여러 제거 요청은 집합과 안정적 compaction으로 처리해 Actor 실행 순서 유지.
- Area queue: list 대신 deque, pop 결과 vector 용량 선예약.
- Job: FunctionJob은 기존 일반 할당 유지. 공용 풀과 스레드별 풀 모두 시험했으나 최종 적용에서 제외.
- Job: dispatcher 완료 카운터가 0이 되는 경우만 대기 mutex 획득. 대기 알림은 mutex 안에서 수행해 lost wake-up 방지.
- Job: Actor mailbox의 0→1 실행 소유권 프로토콜과 기존 TLS 1개 연기 정책 유지.

공용 FunctionJob 풀·작업 가져오기·TLS 8개 연기를 함께 적용한 초기 시험에서 회귀가 관찰됐다. 스레드별 풀로 바꾼 시험에서도 기존 측정보다 느린 조건이 반복돼 최종 구현에서는 풀, 작업 가져오기, TLS 확대 모두 제외했다. 개별 원인 분리가 완료된 것은 아니므로 특정 기법 자체가 항상 느리다고 결론내리지 않는다. 초기 공용 풀 실행 기록은 `artifacts/scenarios-optimized`에 남고 최종 결과와 섞지 않는다.

작업 가져오기와 TLS 확대를 포함했던 첫 3방식 시험은 `artifacts/scenarios-three-way-experimental`에 보관한다. 최종 적용 구성의 3방식 결과는 `artifacts/scenarios-three-way`이며 두 결과를 합산하지 않는다.

스레드별 풀을 포함한 재시험은 `artifacts/scenarios-three-way-pool-experiment`에 보관했다. S10 일부까지 실행 후 중단한 자료이므로 완결된 전체 비교로 취급하지 않는다.

## 의도적으로 유지한 것

- Actor 순서에 영향을 주는 swap-remove는 사용하지 않음.
- 반경/시야 보호를 제거하거나 다른 partition을 동시에 실행하지 않음.
- 운영 타이머 Task의 객체 재사용은 적용하지 않음. 현재 Worker가 실행 후 delete하므로 예약 소유권·종료 취소를 함께 재설계해야 함.
- 업데이트 검증용 atomic은 유지. 검증 제거에 따른 측정 왜곡 방지를 위해 세 방식에 같은 검증 적용.
- 가산 damage를 합치는 이벤트 병합은 적용하지 않음. 이벤트별 실행 의미를 유지.
- `std::function`을 전용 DamageJob으로 교체하는 작업은 이번 범용 할당 재사용과 분리해 추후 검토.

## Git 원본

기준 커밋은 `b713018`이다. `Source/AreaUpdateLegacy/Baseline/b713018/Source`는 Git archive로 추출한 원본이며 수정하지 않는다. `OriginalAreaAdapter`는 이름 공간을 분리해 원본 Area와 AreaExecuter 구현을 그대로 컴파일한다.

저장소의 `core.autocrlf=true`로 추출본은 CRLF다. Git의 줄바꿈 정규화를 적용한 blob hash가 원본 7개 파일 모두 커밋과 일치함을 확인했다.

원본의 반경 lock, random queue, head 충돌 중단, 전체 Area 실행을 유지한다. 원본에 없던 Actor 목록은 파생 Area에서 제공하며, 반경 2의 참조 연결을 어댑터에서 구성한다. 매 논리 tick 전체 Area를 등록하고 원본 실행기로 모두 처리한 뒤 다음 tick으로 진행한다.

따라서 비교는 원본 서버 main/Worker의 50ms 루프를 그대로 실행하는 것이 아니라, **원본 공간 lock/queue 알고리즘과 현재 공간 스케줄링 및 Actor Job의 공통 부하 시험**이다. 현재 Area도 비교용 partition barrier 실행을 사용한다. 세 방식 모두 같은 JobDispatcher를 공통 실행 기반으로 사용한다.

## 시나리오와 재현

기존 S01~S13은 `../testing.md`와 같다. S14는 640×640 맵, Actor 20,000개 균등 분포, 상호작용 8, 연산 1,000회, Worker 8개다. S15는 같은 조건에서 연산 20,000회다. 공간 계산은 `../spatial-scheduling.md`에 설명한다.

```powershell
.\tools\benchmark\Run-ScenarioComparison.ps1 -WithOriginal
# 일부 시나리오만 선택
.\tools\benchmark\Run-ScenarioComparison.ps1 -WithOriginal -Only 'S04,S05,S06,S10,S11,S14,S15'
```

측정은 각 조건 100 tick, 별도 World 준비 실행 5 tick, 방식별 3회다. 세 방식의 실행 순서를 회전한다. checksum, Actor별 수신 damage와 업데이트 횟수, 동시 접근 검증을 유지한다. 결과는 `artifacts/scenarios-three-way`에 저장한다. 원본과 수정본은 초기화/메모리 자료구조가 다르므로 CPU·경과시간과 메모리 사용량을 구분해서 해석해야 한다.
