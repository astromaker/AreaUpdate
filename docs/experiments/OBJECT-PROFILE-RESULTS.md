# Object 기준·전달 Task 제거 후보 프로파일 결과

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

## 결론

28만에서는 **Task 수명 관리와 힙 할당·해제가 유의미한 비용**임을 확인했다.
전달 Task 제거 후보는 힙 관련 own 샘플 비중이 약 20.06% → 15.78%로 줄었고,
프로파일 실행에서도 두 회차 모두 높은 처리량을 보였다.
다만 후보는 Dispatcher 큐도 바뀌었으므로 개선 전체를 할당 제거 하나에 귀속하지 않는다.

7만은 context switch가 매우 잦고 스택 수집 자체의 비용이 커,
이전에 관측한 약 6% 회귀의 원인을 이번 기록만으로 확정하지 못했다.
기준/후보의 프로파일 FPS 순서도 회차마다 달라졌다.
기존 범용 채택 보류 결정을 뒤집지 않고, 7만은 가벼운 기록으로 추가 확인하는 것이 적절하다.

## 자료와 검증

- `artifacts/object-profiles-admin`: 7만/28만 × 기준/후보 × seed 42/43, 총 8개 ETL.
- 기준 EXE SHA256: `882C22FF194AF67CA7E4245D28B8B388AF0116265BC19D4080D682727A169356`.
- 후보 EXE SHA256: `2AD9F6E586BC07446F9A62AA9F23F373F7EC283F36D7C73E910D4DE51BCD2BF2`.
- 각 기록의 EXE 해시와 stack report PID 검증, EXE/PDB GUID·Age 일치 확인.
- 8개 모두 Lost Events=0, Lost Buffers=0. CPU own 샘플 합과 프로세스 샘플 총합 일치.
- 실행 종료 로그에서 overlaps=0, neverUpdated=0, tasks_live=0, tasks_pending=0 확인.
- CPU 함수 이름은 해석됐지만, 최적화에 의한 inline/COMDAT 병합은 별도 제약이다.
- CSwitch 스택 보고서는 `KeExpandKernelStackAndCalloutInternal` 한 함수로 축약돼,
  대기 원인을 소스의 특정 mutex/CV까지 연결하는 데에는 사용하지 않았다.
- 실행 정책과 C++는 변경하지 않았다. 분석 코드와 가벼운 추가 수집 설정만 추가했다.

## CPU own 샘플 비중

각 값은 두 회차 비중의 산술 평균이다. 프로세스 CPU 샘플 안의 비중이지 장비 전체 CPU 사용률이 아니다.
own/exclusive끼리만 합산했으며 inclusive를 더하지 않았다.

| 항목 | 7만 기준 | 7만 후보 | 28만 기준 | 28만 후보 |
|---|---:|---:|---:|---:|
| 힙 할당·해제 함수군 | 8.37% | 6.57% | **20.06%** | **15.78%** |
| 스택워킹·추적 관련 함수군 | 30.88% | 33.07% | 1.08% | 1.41% |
| RtlSleepConditionVariableSRW | 9.74% | 9.48% | 1.19% | 1.48% |
| TestObject::update own | 2.40% | 2.51% | 5.09% | 6.60% |

힙 함수군은 Heap/malloc/free/operator new/delete 이름의 own 샘플을 분류한 것이다.
예를 들어 28만 seed 42 기준에서 `RtlpLowFragHeapAllocFromContext` own은 11.65%, `RtlFreeHeap`은 5.83%였다.
실행기 전달 Task의 삭제 destructor도 별도로 약 4.02%였지만, 이 함수에는 shared_ptr·통계 처리도 섞여 있어
이를 전부 힙 비용으로 더하지 않았다.

`RtlSleepConditionVariableSRW`의 own 비중은 그 함수에서 실제 실행 중 잡힌 CPU 샘플이다.
스레드가 잠든 벽시계 시간과 동일하지 않다.
TestObject도 실제 게임 로직이 아니라 시각·중복 실행·갱신 간격 계측을 포함한 예제다.

## 28만: 전달 경로 개선 효과는 확인

프로파일을 켠 30초 전체 실행의 정상 구간 FPS다. CPU 기록 구간만의 FPS는 아니며,
기존 20초 비계측 성능 표에 섞어 쓰지 않는다.

| seed | 기준 FPS | 후보 FPS |
|---:|---:|---:|
| 42 | 8.51 | 10.57 |
| 43 | 8.41 | 10.38 |

전체 실행 CPU 스택의 inclusive 비중은 다음처럼 이동했다.

| 실행 경로 | 기준 | 후보 |
|---|---:|---:|
| TaskDispatcher::run_worker | 약 56.8% | 약 52.5% |
| Core Worker::run | 약 42.4% | 약 46.8% |

추론: 전달 Task를 없애 Dispatcher 쪽 부담이 줄면서,
Actor마다 남는 타이머 등록·만료 처리 등 Core 경로의 상대적 비중이 커졌다.
각 경로가 정확히 몇 CPU초를 절약했는지나, 다음 단일 병목이 어디인지까지 확정한 것은 아니다.

## 스레드 상태

CPU 시간 상위 16개 native TID를 두 Worker 풀의 분석 대상으로 삼았다.
프로그램 구성은 timer 8개 + Dispatcher 8개이며, native TID와 개별 논리 Worker 번호의 대응은 수집하지 않았다.
초기 200ms 및 신뢰할 수 없는 CSwitch 구간을 제외하고, 마지막 실제 스케줄링 이벤트까지만 분석했다.
ETL 끝의 rundown 시간까지 임의로 Running/Waiting을 연장하지 않았다.

| Actor / 모델 | Running | Ready | Waiting | Unknown |
|---|---:|---:|---:|---:|
| 7만 기준 | 43.04% | 36.03% | 19.59% | 1.34% |
| 7만 후보 | 41.68% | 27.80% | 29.40% | 1.12% |
| 28만 기준 | 48.40% | 44.87% | 5.45% | 1.27% |
| 28만 후보 | 48.87% | 39.63% | 9.85% | 1.65% |

분모는 **16개 스레드의 시간 합계**다. 논리 CPU 8개이므로 Running 상한은 대략 50%다.
Running에는 그 스레드 문맥에서의 kernel/추적 작업 등이 포함되며, DPC/ISR 시간을 별도 제외한 값은 아니다.
이를 그대로 프로세스 CPU 사용률로 환산하지 않는다.
28만에서는 대부분의 시간에 실행 슬롯이 차 있고, 다른 Worker는 Ready로 기다리는 모습이다.
따라서 이 기록에서 고부하 저성능을 단순한 유휴 sleep 문제로 보기는 어렵다.

28만의 Ready → Running P99는 기준 두 회차 약 31.3/29.9ms, 후보 약 26.2/19.8ms였다.
이는 OS 실행 대기이며 Actor의 전체 갱신 지연이나 Dispatcher 큐 대기와 동일하지 않다.
어느 한 Worker만 모든 부하를 받았다는 증거로도 해석하지 않는다.

## 7만: 추적 교란이 커 원인 확정 보류

- 유효 스케줄링 구간의 대상 Worker switch-out은 초당 약 13.8만~18.1만 회였다.
  28만은 약 0.57만~1.20만 회로 훨씬 적었다.
- 7만 CPU own의 약 30.4~34.5%가 stack walk/unwind/ETW 관련 이름에 잡혔다.
  `EtwpTraceStackWalk`의 inclusive 경로도 이를 뒷받침했다.
- 프로파일 실행 FPS는 seed 42에서 기준 16.22 / 후보 17.23,
  seed 43에서 기준 17.94 / 후보 17.22로 순서가 뒤집혔다.
- 후보가 Waiting 시간을 더 보였지만, 이것만으로 특정 깨우기 버그나 mutex가 회귀 원인이라고 확정할 수 없다.
- 후보의 `RtlAcquireSRWLockExclusive` CPU 샘플 증가도 관측됐으나,
  queue mutex 대기의 전체 비용까지 분리한 결과가 아니므로 약 6% 회귀를 설명한다고 단정하지 않는다.

따라서 이 무거운 기록에서 추적 관련 샘플을 단순히 빼고 원래 성능을 복원하는 계산도 하지 않는다.
실행 순서·배치·깨우기 빈도 자체가 바뀔 수 있기 때문이다.

## 다음 확인

후속 경량 기록과 WPR 없는 재검증을 완료했다. `OBJECT-LIGHT-PROFILE-RESULTS.md` 참고.
경량 기록에서는 회귀가 보이지 않았지만, 비계측 및 Actor 표본 대조에서는 다시 재현됐다.
후보의 Dispatcher 대기는 개선됐고 타이머 꼬리 지연·대기 초과가 악화된 점을 확인했다.

`tools/profiling/ObjectCpuLight.wprp`는 CPU sampling 스택과 CSwitch/Ready 이벤트를 유지하고,
CSwitch/Ready마다의 스택 수집만 제거한다. 7만 기준/후보를 두 번씩 다시 비교하도록 준비했다.
아래는 사용한 후속 수집 명령이다.

관리자 PowerShell 한 명령:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\github\AreaUpdate\Collect-ObjectProfiles.ps1 -LightScheduling -ActorCounts 70000 -OutputDirectory artifacts/object-profiles-light-admin
```

가벼운 기록에서도 회귀가 재현되는지 먼저 확인한 뒤,
실행기 직접 전달의 수명 보장을 유지하면서 큐 mutex까지 바꾸지 않는 후보를 검토하는 순서가 적절하다.
atomic별 비중·캐시 미스·false sharing은 이번 자료에서 확인 불가다.

## 분석 자료와 재현

- `tools/profiling/Export-ObjectProfiles.ps1`: 심볼 포함 CPU 함수/스택, thread CPU, Ready, 스케줄링 CSV 추출.
- `tools/analysis/Analyze-ObjectProfiles.py`: 해시·PID·샘플 합·유실 검사와 함수 분류, 스레드 상태 계산.
- `artifacts/object-profiles-admin/analysis-summary.csv`, `comparison.csv`: 요약.
- 각 기록의 `export/object-analysis.json`, `object-functions.csv`, `object-thread-states.csv`: 상세.
- ETL span은 약 12.34~15.06초이며 요청한 CPU 10초와 동일하지 않았다.
  `etlSpanNormalizedCpuPercent`는 rundown을 포함한 span 분모의 참고값일 뿐, 성능 비교 CPU로 사용하지 않았다.
- 함수 주소가 공유되는 COMDAT 병합 때문에 job 실행에서 LegacyTimerTask 같은 별칭이 보일 수 있다.
  이를 실제 Legacy 모델 실행으로 해석하지 않았으며, 클래스별 Task 비용을 정확히 분리하지 않았다.
- 분석기 자체 검사는 Running/Ready/Waiting 예제, 경계 자르기, 미관측 스레드·histogram 경계 등을 확인한다.
