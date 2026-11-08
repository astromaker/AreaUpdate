# 현재 radius 모델 CPU·HOL·타이머 분석

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

## 결론

이번 자료에서는 Worker 깨우기나 timer Task 개수보다 **Area/Actor 상태 접근과 radius mask 접근의 CPU 비용**이 더 컸다. 긴 큐 대기 중에도 주요 Worker들은 대부분 실행 중이었다. 타이머 최악 기록은 녹화 시작 경계의 대기 복귀 구간에 있었고, 긴 timer callback 때문은 아니었다.

실제 radius mask 해제 시각, 캐시 미스/false sharing의 비율, 시작 경계 지연의 정확한 원인까지 확정된 것은 아니다. 이번 작업은 분석이며 C++ 실행 로직을 수정하지 않았다.

## 자료 검증

- `artifacts/radius-profiles-admin`의 capacity/hol/latency 모두 mode=legacy였다.
- 세 실행 모두 Lost Events/Buffer 0, overlap 0, never_updated 0, live Task 0이었다.
- 실행 파일/PDB의 보관 해시를 재검증했다. 세 실행 파일 SHA256은 동일한 `7EDDB123BD83024877AB668508AFFD4E1D24F79649182C046F1CC1CD17F64F25`이며 현재 Release 파일과도 일치한다.
- Microsoft 공식 서버의 OS/런타임 심벌을 별도 작업 폴더에 받아 재해석했다. capacity의 자체 샘플 중 미해석은 158/79,465개, 약 0.20%였다.
- 원래 내보내기와 심벌 재해석 보고서의 스택 총수가 조금 다르므로 수치를 섞지 않았다. 아래 CPU 비율은 재해석 보고서 기준이다. 자체 샘플의 합이 해당 보고서의 프로세스 총수와 일치함을 검사했다.

| 사례 | Actor / 맵 | 추가 진단 | 기록 요청 / ETL 헤더 범위 | 프로세스 실행 |
|---|---|---|---:|---:|
| capacity | 280,000 / 640×640 | OFF, timer trace OFF | 10초 / 14.482초 | 25초 |
| hol | 280,000 / 640×640 | HOL ON, Area 1/4096 표본, timer trace 10ms | 10초 / 12.336초 | 25초 |
| latency | 5,000 / 64×64 | timer trace 10ms | 30초 / 30.971초 | 45초 |

정지·후처리 구간과 스택 보유 여부가 다르므로 샘플 수를 요청한 10초로 나눠 처리량을 계산하지 않았다. 실행 로그의 FPS/히스토그램도 전체 실행 또는 별도 정상 구간 값이며 ETL 범위와 같지 않다. 프로파일링 자체의 비용이 있으므로 일반 성능 A/B 결과와 합산하지 않는다.

## 1. 고부하 CPU 호출 경로

대상 PID 6824의 CPU Profile 스택 79,465개 기준이다. CSwitch/ReadyThread 스택 개수를 CPU 점유율로 사용하지 않았다.

| 함수 | 자체 샘플 | 하위 호출 포함 |
|---|---:|---:|
| Area::update_at | 28.31% | 43.23% |
| RadiusArea::try_acquire | 16.21% | 16.22% |
| ntdll!RtlQueryPerformanceCounter | 9.72% | 9.72% |
| TestObject::update | 9.27% | 11.90% |
| Worker::run_timer | 0.41% | 12.23% |
| RuntimeBackend::flush_radius_timers | 0.26% | 4.81% |
| RadiusExecuter::wake_workers | 0.07% | 2.17% |

하위 호출 포함 비율은 서로 중첩되므로 합산하면 안 된다. timer Task/알림 비용이 0이라는 뜻은 아니지만, 이 자료에서 가장 큰 경로는 아니었다.

### 코드와 명령 위치

추가로 raw SampledProfile IP 레코드 100,816개와 보관된 실행 파일의 역어셈블을 확인했다. 이 레코드 집합은 위 스택 보고서와 다르므로 두 분모를 섞지 않았다.

- `Area::update_at`의 집중 위치는 Area 활성 플래그를 읽은 직후(RVA 0x16ad9), Actor 포인터를 읽은 직후(0x16cd7), Actor deadline 필드를 읽은 직후(0x16ce0)였다. 각각 raw IP 8,853/5,545/10,823개였다.
- `RadiusArea::try_acquire`에서는 mask 목록 메타데이터를 읽는 진입부(0x246eb), 공유 mask word 읽기(0x2474d), `lock cmpxchg` 직후 분기(0x2476b)에 집중됐다. 각각 5,884/7,052/2,285개였다.
- 이는 상태/포인터/atomic 접근 주변이 뜨겁다는 근거다. 주기적 IP 샘플만으로 해당 명령의 정확한 stall cycle, 캐시 미스, false sharing 비율을 확정하지는 않았다.
- `areaRunCount`는 실행 로그에서 disabled였다. 예전 전역 계측 카운터 비용을 이번 핫스폿 원인으로 돌릴 수 없다.

시간 조회는 `_Query_perf_counter`를 거쳐 `RtlQueryPerformanceCounter`로 연결됐다. 호출 경로에는 `steady_clock::now`, Area tick, 테스트 Actor의 갱신 간격 측정 등이 포함된다. 따라서 9.72%를 전부 게임 계산이나 scheduler만의 비용으로 해석하면 안 된다. `TestObject`는 현재 연산 횟수 0인 예제이며 시각 조회·중복 검사·갱신 횟수·간격 히스토그램을 처리한다.

`RtlpAcquireSRWLockExclusiveContended`도 하위 호출 포함 6.11%였으나 여러 mutex 경로의 합이다. 이것을 특정 Area mutex나 Worker 목록 잠금 한 곳의 비용이라고 단정하지 않았다.

## 2. HOL 대기와 Worker 실행 상태

hol 실행 로그 전체의 Area 표본은 19,123개였다. 게시 후 첫 시도 전 평균은 39.685ms, 관측된 HOL 창과 겹친 평균은 28.955ms였다. 이는 이전의 관측 범위와 비슷하다.

ETW raw QPC와 상대 시간 사이의 변환을 원자료로 계산했다. 주파수는 10MHz였으며 검증한 시각 쌍의 counter 변환 오차는 0ns였다. 이는 Core 로그의 ms 정밀도가 ns로 개선됐다는 뜻은 아니다. MSVC의 현재 steady_clock 구현도 QPC 기반임을 설치된 헤더에서 확인했다. 따라서 Area 기록과 ETW 시각을 단순 boot-time 추정이 아니라 동일 counter 기준으로 맞췄다.

CPU 누적 상위 8개 주요 스레드에 대해 CSwitch/ReadyThread로 상태 구간을 재구성했다. 관측된 완결 구간의 전체 비율은 실행 중 약 88.8%, Ready 약 6.4%, Waiting 약 3.6%, 나머지는 미확정 상태 등이었다. 구간 경계와 부분 기록은 분리했다. Running에는 spin 및 ETW 자체의 커널 처리도 포함될 수 있어 전부 유용한 Actor 계산이라는 뜻은 아니다.

실제 긴 Area 큐 대기 구간 세 개를 첫 시도 시각과 맞춰 보면 다음과 같다. 녹화 초기 200ms 및 요청 기록 범위 밖의 표본은 제외했다.

| Area 좌표 | 첫 시도 전 대기 | 8개 주요 스레드 Running | Ready | Waiting |
|---|---:|---:|---:|---:|
| (398,426) | 78.647ms | 83.81% | 10.66% | 3.54% |
| (244,220) | 77.456ms | 83.57% | 10.13% | 4.20% |
| (212,124) | 75.865ms | 83.79% | 10.34% | 3.97% |

따라서 이 표본의 긴 큐 대기를 'Worker가 대부분 sleep해서 생겼다'고 설명할 수 없다. 실행 중인 다른 작업, 큐 순서, 공간 충돌과 재방문 간격을 함께 봐야 한다.

**남은 확인 불가:** radius lock은 사용자 공간 atomic bitmask다. 현재 ETW 이벤트만으로 특정 head의 모든 mask가 동시에 자유로워진 정확한 시각은 알 수 없다. 그러므로 HOL 창의 28.955ms를 순수 잠금 불가 시간으로 볼 수 없고, 실제 해제 이후 재시도까지의 시간도 아직 정확히 분리하지 못했다. OS Running/Ready/Waiting 분류와 '게임 공간 잠금이 자유로운 상태'는 다른 개념이다.

## 3. 타이머 순간 지연

latency 실행은 평균 정상 구간 19.994FPS, 전체 관측 최대 Actor 간격 111ms, 최대 timer 지연 48ms였다. 보관된 timer trace 256개는 Worker당 상위 32개이며 전체 45초 실행에서 선택됐다.

48ms 사례의 분해는 다음과 같다.

| 구간 | 시간 |
|---|---:|
| deadline → 휠 실행 가능 시점 | 4ms |
| 실행 가능 시점 → 다음 timer poll | 44ms |
| poll → 선택 | 0ms |
| 선택 → 해당 callback 실행 | 0ms |

해당 기록의 wait_for_work 전후 시각도 48ms였고 sleep 종료와 poll은 같은 ms였다. 이 사례는 앞선 callback이 48ms 계산해서 생긴 지연이 아니다. ms 단위 기록이므로 0ms는 '1ms 미만일 수 있음'을 뜻한다.

이 실행 시점은 ETW 시작으로부터 약 44.648ms였다. 같은 초기 구간의 주요 native 스레드 17100/8552에는 각각 31.805/31.691ms의 Waiting과 그 뒤 0.024/0.021ms의 Ready→실행 지연이 관측됐다. Core Worker 인덱스와 native TID를 직접 매핑한 자료는 없으므로 두 기록을 같은 작업이라고 단정하지 않았다. 다만 이 구간의 긴 지연을 전부 Ready 상태의 CPU 재할당 대기라고 설명할 수도 없다.

녹화 시작 경계이므로 추적 활성화/커널 스택 수집 영향과 일반 동작을 구분해야 한다. 저부하 CPU 자체 샘플에서도 커널 stack walk 관련 함수가 상당 부분을 차지했다. 이 자료만으로 특정 condition_variable 구현이나 Windows timer resolution을 단일 원인으로 확정하지 않았다.

보관된 최악 timer 기록은 녹화 시작 직후 또는 ETL 범위 밖에 집중돼 있었다. 이는 상위 K 보관 정책과 같은 지연 값의 교체 조건에 영향을 받으므로 '녹화 중 지연이 없었다'는 증거는 아니다. 전체 실행의 max/히스토그램과 ETL 시간 범위를 혼동하면 안 된다.

## 다음 수정 후보

1. Area/Actor 상태 접근을 먼저 독립 A/B 대상으로 삼는다. 중복 활성 검사, Actor 포인터·deadline 조회, 콜드 데이터 배치를 살피되 갱신 보장과 소유권을 바꾸지 않는다.
2. radius의 immutable mask 메타데이터와 atomic word 접근 배치를 검토한다. 단순 CAS 횟수/공간 대기/캐시 공유 비용을 구분해야 하며, 이번 결과만으로 padding 등의 효과를 보장하지 않는다.
3. 시간 조회 9.72%에서 테스트 계측과 실제 scheduler 비용을 분리한다. 성능을 좋아 보이게 하려고 실제 callback 시각 측정을 부정확한 이전 시각으로 대체하면 안 된다.

Worker burst, 알림 확대, Task 병합을 반복하는 것보다 이 순서가 이번 CPU 자료에 더 직접적으로 근거한다. 현재 분석만으로 새로운 최적화를 기본 적용하지 않았다.

## 재현·검증

- 분석 도구: `tools/analysis/Analyze-RadiusEtw.py`. 표준 HTML/CSV parser로 내보내기를 읽고, C++ 템플릿 이름이 HTML tag처럼 출력된 경우를 처리한다.
- 검증: 실행 파일/PDB 해시, 대상 PID, 유실 0, 자체 샘플 합, 타이머 구간 합, raw counter 시각 변환을 확인했다.
- 도구 단위 검사: 템플릿 이름 보존, 시각 변환, Waiting/Ready/Running 분류를 고정 입력으로 검증했다.
- 결과: 각 사례 `export/stack-tables.json`, `functions.csv`. hol/latency에는 `offcpu.csv`, `timer-paths.csv`가 있고 capacity에는 `cpu-ip.csv`, `area-update.asm`, `radius-acquire.asm`도 있다.
- CPU 심벌 보고서는 세 사례 모두 완결됨을 확인했다. 별도의 시작 구간 전체 스택 dump는 타사 비공개 심벌 조회 지연으로 중단해 `.partial.csv`로 분리했으며, 위 대기 원인의 확정 근거로 사용하지 않았다.

원자료는 `artifacts/radius-profiles-admin/`에 보존했다. C++ 코드나 빌드 설정은 이번 분석에서 변경하지 않았다. 게임의 실제 이동·전투 로직이 제공되지 않은 예제 부하이며 실게임 처리 용량이나 지연 상한의 보장은 아니다.
