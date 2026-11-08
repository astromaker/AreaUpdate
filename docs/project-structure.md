# 프로젝트 구성

## 폴더와 프로젝트

```text
Source/
├─ AreaUpdate.sln
├─ Directory.Build.props
├─ Core/                           공통 실행 기반
├─ AreaUpdate/                     개선된 Area 업데이트
├─ ObjectUpdate/                   Actor별 TaskExecuter
├─ AreaUpdateLegacy/               원본 스냅샷과 어댑터
│  └─ Baseline/b713018/Source/      비교 기준 코드, 수정 금지
├─ Examples/AreaUpdateExample/      Actor 생성·이동 실행 예제
├─ Benchmarks/UpdateComparison/     동일 조건 성능 비교
└─ Tests/AreaTests/                 생명주기·동시성·타이머 검증
```

`Core`, `AreaUpdate`, `ObjectUpdate`, `AreaUpdateLegacy`는 정적 라이브러리다.
나머지 세 프로젝트는 실행 파일이다. 폴더 이름과 프로젝트 이름을 일치시켰다.

## 의존 관계

- Core: Worker, Task, FunctionTask, TaskQueue, PriorityTimer, WheelTimer, WakeSignal.
- AreaUpdate → Core: Area/Actor 소속 관리, AreaManager, AreaExecuter.
- ObjectUpdate → Core: TaskExecuter, TaskDispatcher. 공통 Task와 FunctionTask를 사용한다.
- AreaUpdateLegacy: 원본 Area/AreaExecuter를 Original 이름 공간으로 격리하고 최적화된 radius 실행기를 제공한다.
- AreaUpdateExample → AreaUpdate, ObjectUpdate, AreaUpdateLegacy, Core.
- UpdateComparison → AreaUpdate, ObjectUpdate, AreaUpdateLegacy, Core.

Core는 세 업데이트 구현을 참조하지 않는다. Actor는 현재 Area 소속 관리와 결합되어 있으므로
AreaUpdate에 유지한다. 비교 전용 배치 실행·측정·맵 배치는 Benchmarks에 둔다.
원본 스냅샷의 main/Worker는 별도로 컴파일하지 않으며, 기존처럼 어댑터가 Area/AreaExecuter를 포함한다.

## 공통 Task

기존 Job은 Task로 통합했다. JobExecuter/JobDispatcher/FunctionJob/run_job은
각각 TaskExecuter/TaskDispatcher/FunctionTask/run_task로 변경했다.

- Worker는 Task를 직접 받아 즉시 또는 예약 실행한다.
- TaskExecuter는 Task를 받아 같은 Executer의 작업을 직렬 실행한다.
- TaskDispatcher는 round-robin 배정과 기존 wake-up/완료 대기 정책을 유지한다.
- Worker의 TaskQueue는 기존 atomic 포인터 큐를 사용한다.
- TaskExecuter/TaskDispatcher도 같은 표준 atomic TaskQueue를 사용한다.

Task의 Windows SList 상속과 정렬 매크로는 제거했다. 각 큐는 공통 연결 필드를 사용한다.
동일 인스턴스를 여러 큐·타이머·실행기에 동시에 제출하면 안 된다.
제출한 작업은 실행 측에 소유권을 넘기며, 실행·취소 시 실행 측이 삭제한다.
FunctionTask는 std::function을 감싸며 두 실행 경로에서 모두 사용할 수 있다.

기존 Task의 타이머 필드와 생성·소멸 atomic 통계는 유지되므로, 예전 Job 작업에도 이 비용이 추가된다.
통계는 이제 두 실행 경로의 Task를 모두 집계한다. 과거 성능 결과는 통합 이후 수치로 간주하지 않는다.
Task 통계는 기본 활성이다. 솔루션 빌드에 /p:TaskStatisticsEnabled=0을 주면 생성·소멸 통계를 제외한다.
이 경우 예제는 DIAGNOSTICS task_statistics=0을 출력하며 측정 결과의 Task 수는 null(미측정)로 기록한다.
통계 제외 옵션은 Core를 공유하는 Area에도 동일하게 적용된다. 모든 프로젝트를 같은 설정으로 빌드해야 한다.
TaskDispatcher는 실행기 전달 전용 Task를 사용하며, wait_idle은 작업 소멸까지 포함해 완료를 확인한다.
Worker 번호 선택 카운터만 relaxed를 사용하고 큐 공개·실행권·완료 판정의 동기화는 유지한다.
호환성을 위해 벤치마크 명령행 모드와 CSV의 이름은 기존 job을 유지한다.
원본 서버의 PWJob/PWJobExecuter 이름과 변경 금지 스냅샷은 그대로 유지한다.

Windows 이벤트·고해상도 타이머는 제거했다. TaskDispatcher는 WakeSignal로 대기하며,
WakeSignal은 mutex와 condition_variable로 신호를 보관·병합한다.
Worker는 기본적으로 WakeSignal로 다음 타이머 만료까지 대기하며 새 요청·종료·Area 알림이 깨운다.
알림을 보장하지 않는 Ticker는 1ms 제한시간으로 재확인한다. 예전 Area 어댑터도 이에 해당하며 실제 복귀 시각의 상한은 아니다.
예제의 waitMode 선택 인수로 signal(기본) 또는 poll을 지정해 같은 실행 파일에서 대기를 비교한다.
Worker의 기본 m_pollingWait는 false이며 tools/benchmark/Measure-RuntimeCpu.ps1의 기본값도 signal이다.
poll은 이전 sleep_for(1ms) 경로이며 TaskDispatcher의 대기 방식은 양쪽 모두 동일하다.
과거 실험은 STD-PORT-RESULTS.md와 SLEEP-RESULTS.md에 보존했다.
이번 깨우기 비교 결과와 적용 범위는 WAKEUP-RESULTS.md에 정리했다.
후속 Area 대기 진단은 experiments/AREA-WAIT-DIAGNOSIS.md, signal 기본값 적용 후 두 모델 비교는 SIGNAL-MODEL-COMPARISON.md를 참고한다.
Object의 Actor 실행 Task도 전용 ObjectUpdateTask로 분리했다.
예제의 waitMode 다음 인수는 Actor 제출 대기의 표본 stride이며 기본 0(비활성), 256이면 해시로 약 1/256을 선택한다.
계측은 예제에만 있으며 Actor 직렬 실행과 round-robin을 변경하지 않는다. 비교 결과는 ACTOR-TASK-RESULTS.md에 있다.
실제 Dispatcher Worker 계측은 그 다음 인수로 별도 선택한다. 기본 0이며 256은 Worker별 256회 중 한 번을 표본으로 기록한다.
전달/내부 Task 건수와 batch 벽시계 시간은 전체 집계하며, 큐 대기·개별 실행 시간은 표본 집계한다.
생산자를 중단한 뒤 TaskDispatcher::shutdown으로 join하고 worker_statistics를 읽는다. 결과는 DISPATCHER-BALANCE-RESULTS.md에 있다.
벤치마크 CPU 시간 측정의 GetProcessTimes와 원본 스냅샷은 이번 이식 범위에서 제외했다.

AreaTests는 예외적으로 Core/AreaUpdate 라이브러리를 링크하지 않고 관련 구현을 직접 컴파일한다.
`AREAUPDATE_TEST_HOOKS`가 Worker 클래스 구성에 영향을 주므로, 일반 라이브러리와 섞으면
서로 다른 클래스 정의가 혼용될 수 있기 때문이다. 테스트 전용 컴파일 정의는 테스트 안에서만 유지한다.

## 빌드와 실행

저장소 루트 PowerShell에서 실행한다.

```powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
& $msbuild Source/AreaUpdate.sln /m /p:Configuration=Release /p:Platform=x64
& $msbuild Source/AreaUpdate.sln /m /p:Configuration=Debug /p:Platform=x64

.\Source\x64\Release\AreaTests.exe
.\Source\x64\Release\UpdateComparison.exe --test
.\Source\x64\Release\UpdateComparison.exe all 2000 30 8 1000 8 42 clustered 0
.\Source\x64\Release\AreaUpdateExample.exe 30 10000 2000 42
```

이동 예제의 기본 목표는 Actor 70,000개다. 5초에 걸쳐 순차 입장시키며 1ms 간격으로 입장을 확인한다.
seed 다음 선택 인수는 입장 기간(ms), 그 다음은 입장 확인 간격(ms)이다. 입장 기간 0은 일괄 입장이다.

```powershell
.\Source\x64\Release\AreaUpdateExample.exe 30 10000 2000 42 5000
.\Source\x64\Release\AreaUpdateExample.exe 30 10000 2000 42 0
```

```powershell
# Actor 70,000개, 5,000ms에 걸쳐 입장, 1ms 간격으로 확인
.\Source\x64\Release\AreaUpdateExample.exe 30 70000 2000 42 5000 1
# Actor마다 xorshift 1,000회를 추가해 부하 계측
.\Source\x64\Release\AreaUpdateExample.exe 20 70000 2000 42 5000 1 1000
# 추가 연산 0, Actor 140,000개, 맵 한 변 453 Area
.\Source\x64\Release\AreaUpdateExample.exe 20 140000 4000 42 5000 1 0 453
```

위 설정의 목표 입장률은 약 14 Actor/ms다. 콘솔 인원 출력만 1초 간격이며 입장을 초 단위로 묶지 않는다.
sleep_for(1ms)의 실제 기상은 늦어질 수 있어, 지연되면 경과 시간에 해당하는 인원을 한 번에 따라잡는다.
실제 입장 처리 횟수와 최대 묶음 크기는 SUMMARY의 join_steps/max_join_batch에 기록한다.
즉 정확히 매 1ms마다 실행된다는 보장은 아니다.
tools/experiments/Measure-StdPort.ps1의 런타임 기본 인원도 70,000개다. 과거 10,000개 조건은
-RuntimeActors 10000으로 지정한다. 다른 인원 결과는 별도 접두사로 저장하고
tools/analysis/Summarize-StdPort.ps1 -RuntimeActors 70000으로 해당 결과를 집계한다.

70,000개 검증 실행(30초, seed 42, 이동 시도 2,000회/초, 입장 기간 5,000ms,
확인 간격 1ms)의 평균 Actor FPS는 15.163이었다. 중복 실행·미갱신·잔여 Task는 0이었다.
실제 입장은 417회, 최대 묶음 434개로 기록돼 정확한 1ms 기상이 아님을 확인했다.
로그와 실행 파일은 artifacts/actors-70000에 보존했다.
동일 조건 CPU 재측정(30초 1회, 논리 CPU 8개를 합쳐 100%로 환산)은 평균 10.11%,
입장 완료 후 표본 평균 10.76%, 250ms 표본 최대 17.39%였다.
전체 프로세스 CPU 시간은 24.453초, 평균 약 0.809개 코어 사용에 해당하며 Actor FPS는 15.213이었다.
CPU 수집은 맵 준비·입장·종료도 포함하며, 입장 후 평균은 시작 약 6초 이후 표본으로 계산했다.
이는 Worker busy/idle 벽시계 시간과 다른 실제 프로세스 CPU 시간 측정이다.
tools/benchmark/Measure-RuntimeCpu.ps1로 재현하며 로그·summary.json·samples.csv는 artifacts/actors-70000/cpu에 있다.
재측정할 때는 -OutputDirectory로 새 폴더를 지정해 기존 결과를 보존한다.
후속 20 FPS 부족 원인 계측과 CPU 한계 탐색은 CAPACITY-RESULTS.md를 참고한다.
Actor 수 증가 실험은 ACTOR-SCALE-RESULTS.md에 정리했다. 마지막 선택 인수 mapSide는
한 변의 Area 수이며 0 또는 생략 시 기존 기본 맵을 사용한다.
mapSide 다음 선택 인수로 area(기본), original, job 모드를 지정할 수 있다.
예제의 비교 어댑터는 RuntimeBackend.h에 있으며 세 라이브러리를 연결한다.
Core/AreaExecuter의 실행 정책은 이 모드 추가로 바꾸지 않았다.
분산 입장·이동을 포함한 비교는 RUNTIME-THREE-WAY-RESULTS.md를 참고한다.
후속으로 job 모드를 Actor별 고정 50ms 주기 예약으로 변경했고, 현재 area 모드에도 같은 정책을 적용했다.
Worker::add_task_at은 get_now_tick과 같은 절대 밀리초 시각으로 제출하는 API다.
현재 job 모드와 이전 완료 기준 예약의 1만 Object 비교는 FIXED-PERIOD-RESULTS.md에 있다.
Core/UpdateTickPlan.h에서 이전 예정 시각 +50ms 및 지난 예약 건너뛰기를 공통으로 계산한다.
Area는 활성 예약 시작 때 기준을 잡으며 예약이 끝난 뒤 재활성화되면 새 기준을 사용한다.
original 모드와 변경 금지 스냅샷은 완료 기준 예약을 유지한다.
변경 직전 Area·고정 주기 Area·현재 Object 비교는 AREA-FIXED-COMPARISON.md에 정리했다.
공통 타이머 휠은 256슬롯·5ms를 사용한다. 1ms 비교 후 기본 설정을 5ms로 복원했다.
마지막 선택 인수로 타이머 지연 추적 임계값을 지정할 수 있다. 기본 0은 비활성이고 50은 50ms 이상이다.
Worker별 상세 기록은 32건 상위 지연으로 제한하고 join 후 출력한다.
비교와 큰 지연 사례 분석은 WHEEL-RESOLUTION-RESULTS.md를 참고한다.
빌드 직후 Debug 회귀 테스트는 1회 실패했고 원인은 확인하지 못했으나,
즉시 재실행과 추가 5회는 모두 통과했다. 예제 외 구현·검증 조건은 변경하지 않았다.

실행 시간은 맵 준비 후부터 측정한다. 모든 Actor의 정상 갱신을 보려면 입장 기간보다 충분히 길게 실행한다.
실행이 먼저 종료되면 SUMMARY의 actors는 실제 입장 수, target_actors는 목표 수다.
actor_fps는 각 Actor가 입장한 이후의 시간 합계(actor_seconds)를 분모로 계산한다.
입장만 약 1ms 간격으로 확인하며, 기존 이동 시도는 초당 묶음으로 유지한다.
이는 실행 예제의 입장 부하 모델 변경이며, UpdateComparison의 고정 인원 비교 시나리오는 변경하지 않는다.

실행 파일·라이브러리는 `Source/x64/<구성>/`, 중간 파일은 그 아래 프로젝트별 폴더에 생성한다.
공통 빌드 설정은 Directory.Build.props에 두고, 구현별 헤더 검색 경로와 참조는 각 프로젝트에서 지정한다.
일반 라이브러리와 예제의 C++ 언어 수준은 기존 기본 설정을 유지하고, 테스트와 벤치마크는 C++17이다.
솔루션의 x86 구성은 기존처럼 테스트·벤치마크를 x64에 연결하므로, 전체 검증에는 x64를 사용한다.

Visual Studio에서는 AreaUpdateExample을 시작 프로젝트로 지정한다.
Debug x64를 선택하고 실행 인수를 지정한 뒤 F5로 디버깅한다.
VS Code의 C++ 설정에서는 해당 실행 파일의 절대 경로를 디버거 program으로 지정하면 된다.

## 비교 기준 유지

모드 이름 `area / original / job / single / all`과 명령행 인수는 변경하지 않았다.
예전 AreaUpdate.exe 실행 예제는 이제 AreaUpdateExample.exe다.
과거 측정 문서·artifacts에 기록된 실행 파일 이름과 해시는 당시 결과이며 소급 변경하지 않는다.
이전 빌드에서 남은 AreaUpdate.exe가 있다면 이번 예제로 오인하지 않도록 주의한다.

스냅샷은 경로만 이동하고 바이트는 유지한다. 포맷·명명 검사에서 AreaUpdateLegacy를 제외한다.
폴더 분리에 따른 빌드·검증만 수행하며, 기존 성능 결과가 새 실행 파일의 측정 결과를 뜻하지는 않는다.
