# Actor Task와 Area 비교

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

분산 입장·이동·실제 Core 타이머를 포함한 후속 실험은 RUNTIME-THREE-WAY-RESULTS.md에 있다.
아래는 당시의 고정 tick 비교 기록이며 두 실험의 수치를 같은 측정으로 해석하지 않는다.

Source/AreaUpdate.sln에 포함한 UpdateComparison은 소유권과 빈번한 Actor 간 상호작용을
통제된 조건에서 비교한다. 기존 AreaUpdate 애플리케이션을 대체하는 실행 파일은 아니다.

## 이식과 이름

실행기는 D:/github/Server/server3/Library/Core를 참고해 이식했다.

| 원래 이름 | 프로젝트 이름 |
|---|---|
| PWJobExecuter | TaskExecuter |
| PWJob / PWQueue | Task / TaskQueue |
| runJob / tryExecute / popAll | run_task / try_execute / pop_all |
| PWThreadManager dispatch | TaskDispatcher |

클래스는 PascalCase, 함수는 Worker/AreaQueue와 같은 snake_case, 비공개 멤버는 m_camelBack이다.
Executer 철자는 AreaExecuter와 맞췄다. 원본 MIT 고지는 ThirdParty/ServerCore.LICENSE에 보존했다.

intrusive atomic 큐의 등록·목록 분리·역순 처리, 미완료 작업 수와 0→1 실행권 전환,
실행기 차례당 분리한 목록 하나 처리, 중첩 실행기의 TLS 지연 처리를 유지했다.
카운트에는 반영됐지만 큐에 아직 공개되지 않은 작업은 유휴로 판정하지 않고 재시도한다.
공유 소유권은 큐에서 기다리는 실행기의 수명을 유지한다.

PWRefPointer는 std::shared_ptr로, PWDoAsync 템플릿은 FunctionTask로 대체했다.
당시 모든 비교 모드는 알림으로 깨우는 동일한 TaskDispatcher 풀을 사용했다.
사용하지 않는 PWQueue 카운트와 예전 스레드 그룹·할당 풀·잠금 스택 연동·타이머 도우미·서버 의존성은
가져오지 않았다. 따라서 원래 Server 런타임 전체가 아니라 이식한 구현을 측정한 결과다.
콜백 예외는 즉시 실패로 처리한다. 작업은 예외를 던지거나 다른 Actor를 기다리며 차단하면 안 된다.
Dispatcher는 실행기와 모든 작업 제출보다 오래 살아 있어야 한다.

## 동일 부하

- 변경하지 않는 320×320 Area 맵과 대각선을 포함한 반경 2를 사용했다.
- 모든 Actor는 논리 tick마다 같은 결정적 정수 연산으로 한 번 갱신된다. 위치는 seed로 생성했다.
- 상호작용은 실제 가까운 Actor에게 결정적 피해량을 더한다. 고립된 Actor는 대상을 만들지 않는다.
- job 모드는 Actor마다 TaskExecuter 하나를 가지며 피해량은 수신 Actor의 메시지 Task로 전달한다.
- area 모드는 실제 Area::update()로 Actor 목록을 갱신하고 피해량을 직접 반영한다.
  겹치지 않는 반경 2 영역을 25색으로 나누고 색상 완료 후 다음 색상을 실행한다. batch는 최대 64개다.
- single은 직접 순차 갱신하며 정확성과 직렬 실행 비용의 기준이다.
- tick 경계에서 모든 작업과 메시지가 끝난 뒤 진행한다. 수신 피해·갱신 횟수·모드별 checksum을 검증한다.
- 피해 처리를 공유 상태 atomic으로 대체하지 않는다. 공통 접근 검사는 겹치는 접근을 발견하면 실패시킨다.

이 비교는 tick마다 명시적인 색상 장벽을 사용하며 실제 AreaExecuter의 비동기 타이머·큐 회전과 다르다.
메시지도 tick 경계에서 모두 처리해 같은 tick 조건의 상호작용·소유권 모델을 분리해 비교한다.
PERFORMANCE.md의 비동기 수치를 그대로 재현하지 않는다. 고정 위치와 덧셈 피해는 메시지 순서와 무관한
최종 상태를 만든다. 이동·AOI 관리·변경 중인 이웃 읽기·여러 Actor의 거래는 이 부하로 검증되지 않았다.

## 빌드와 실행

MSBuild로 솔루션을 Release x64 빌드한 뒤 저장소 루트에서 실행한다.

```powershell
.\Source\x64\Release\UpdateComparison.exe --test
.\Source\x64\Release\UpdateComparison.exe all 2000 30 8 1000 8 42 clustered 0
.\tools\benchmark\Run-Comparison.ps1
```

위치 인수:

```text
mode actors ticks interactions workIterations workers seed layout periodMs [warmupTicks]
```

- mode는 당시 all, job, area, single을 비교했다.
- layout은 uniform(전체 맵), clustered(중앙 64×64), hotspot(Area 하나)이다.
- periodMs=0은 처리량, 50은 목표 20Hz와 놓친 tick 수를 측정한다.
  초과 시 다음 기한을 재설정하며 무제한 따라잡기는 하지 않는다.
- 연산 횟수는 데이터 의존적인 xorshift 반복이며 마이크로초 단위 실행 시간이 아니다.
- warmupTicks 기본값은 0이다. 지정 시 같은 배치의 별도 World로 예열하고 측정 World를 새로 구성한다.
- cpu_seconds와 cpu_one_core_percent는 run() 동안의 프로세스 CPU로 준비·예열·검증을 제외한다.
- seconds는 준비·이웃 구성·검증·해제를 제외한다. 주기 실행의 의도적 대기는 포함하지만 p99_tick_ms는 제외한다.
- updates_per_sec는 원본 Actor 갱신 수다. events는 수신 메시지 또는 직접 피해 호출을 따로 센다.

자체 테스트는 동시 생산자·모든 배치·고립 Actor·모드 간 일치·접근 중복 검출을 확인한다.
당시 tools/benchmark/Run-Comparison.ps1은 상호작용과 배치 조합을 세 번씩 single/job/area 고정 순서로 실행하고
artifacts/comparison/summary.csv에 저장했다. 이 스크립트에는 예열·무작위 순서가 없었다.
확장성을 판단하려면 긴 실행·여러 연산 크기·1/2/4/8 Worker 조건이 필요하다.
hotspot 하나는 의도적으로 Area 간 병렬성을 없앤다.

## 당시 검증

- Debug/Release x64 솔루션 빌드와 양쪽 자체 테스트 통과.
- 동시 생산자 4개가 실행기 하나에 4,000개 작업을 제출한 뒤 모든 배치와 모드 일치 확인.
- 기존 AreaTests 통과.
- Release: Actor 2,000개, 30tick, 연산 1,000회, Worker 8개, seed 42.
  clustered/hotspot × 상호작용 0/8/32 × 3회, 총 54개 모드 실행에서 수신 상태·checksum 일치와 중복 접근 0 확인.
- 원본 결과는 artifacts/comparison/summary.csv에 보존했다.
- Worker 1개와 주기 50ms 추가 실행에서도 세 모드가 일치했다.

짧은 실행은 시험 도구 검증이다. 충분히 예열한 장시간 확장성 결과나 원래 PW 런타임 성능은 아니다.
후속 시나리오와 규칙은 [시험 시나리오](../testing.md)를 참고한다.
tools/benchmark/Run-ScenarioComparison.ps1은 별도 프로세스·예열·실행 순서 교대·중앙값 집계를 사용한다.
