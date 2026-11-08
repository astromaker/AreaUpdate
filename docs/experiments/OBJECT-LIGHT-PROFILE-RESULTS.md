# 7만 Object 경량 프로파일·대조 측정

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

## 결론

가벼운 WPR 기록만 보면 후보가 더 빨랐지만, WPR을 끈 대조 측정에서는 약 4% 회귀가 다시 나타났다.
따라서 기존 7만 회귀를 잘못된 결과라고 폐기하지 않는다. 전달 Task 제거 후보의 기본 미채택 결정을 유지한다.

이어 WPR 없이 Actor 일부만 표본 계측한 결과:

- 후보의 Dispatcher 평균 대기와 P99는 개선됐다.
- 반면 타이머 지연 P99와 기한을 넘겨 대기에서 돌아온 횟수가 증가했다.
- 50ms 고정 주기 예약에서 건너뛴 tick도 늘었다.

즉 이번 회귀에서 확인된 악화 구간은 **Actor 전달 이후의 큐 평균 대기보다 타이머 쪽 꼬리 지연**이다.
왜 대기 초과가 증가하는지는 아직 OS timeout·실행 배정·신호/잠금 경쟁으로 분리하지 못했다.
큐 mutex 하나를 원인으로 단정하거나, 타이머 누락 정책을 바꿔 해결됐다고 볼 수는 없다.

## 공통 조건과 검증

- 기존 기준/후보 보관 EXE를 그대로 사용했다. C++ 코드와 실행 정책 변경 없음.
- EXE SHA256은 앞선 보고서와 동일: 기준 `882C22FF194AF67CA7E4245D28B8B388AF0116265BC19D4080D682727A169356`,
  후보 `2AD9F6E586BC07446F9A62AA9F23F373F7EC283F36D7C73E910D4DE51BCD2BF2`.
- Actor 70,000, 맵 320, 이동 시도 초당 2,000, seed 42/43, 1초/1ms 분산 입장.
- signal 대기, 고정 주기 20FPS, 5ms × 256 타이머 휠, Task·Worker 공통 진단 ON.
- CPU 표본 스택과 CSwitch/Ready 이벤트는 유지하고 CSwitch/Ready 스택만 제외한 ETL 4개.
- 4개 기록 모두 Lost Events=0, Lost Buffers=0. PID·EXE 해시·CPU own 샘플 합 검사 통과.
- WPR 대조 측정 4회, Actor 표본 대조 4회를 추가했다. 추출·분석·다른 빌드를 병행하지 않았다.
- 모든 대상 실행에서 overlaps=0, neverUpdated=0, tasks_live=0, tasks_pending=0 확인.
- ‘추가 계측 OFF’도 기존 Task·Worker·Actor 공통 진단은 남아 있다. 완전한 무계측 빌드는 아니다.

## 1. 가벼운 WPR 기록

| seed | 기준 정상 FPS | 후보 정상 FPS |
|---:|---:|---:|
| 42 | 19.866 | 19.933 |
| 43 | 18.971 | 19.980 |

위 FPS는 30초 실행의 정상 구간 값이며 CPU 기록 구간만의 FPS는 아니다.
이 값만으로 후보가 기본판보다 빠르다고 결론 내리면 안 된다.

스택워킹·ETW 관련 own 샘플은 이전 무거운 기록의 약 30~35%에서 이번에는 약 0.94~1.59%로 줄었다.
기록 비용이 0이 된 것은 아니며, 이벤트 추적과 CPU sampling 자체는 계속 실행됐다.
이번 기록에서도 기준 seed 43은 CV 함수 own 비중 약 21.98%와 높은 전환 빈도를 보였다.
다른 세 기록의 CV own 비중은 약 11.74~13.02%였다. 실행별 변동이 크다.

| 스레드 시간 비율 | 기준 seed 42 | 후보 seed 42 | 기준 seed 43 | 후보 seed 43 |
|---|---:|---:|---:|---:|
| Running | 31.21% | 31.06% | 47.09% | 28.86% |
| Ready | 19.38% | 25.16% | 28.58% | 17.51% |
| Waiting | 48.44% | 42.66% | 23.47% | 52.83% |

잔여 약 1%는 Unknown이다. 분모는 16개 Worker의 시간 합계이며 논리 CPU는 8개라 Running 상한은 대략 50%다.
native TID와 timer/Dispatcher 개별 Worker 번호의 대응을 수집하지 않았으므로 풀별 대기로 분리하지 않았다.
이벤트 rundown은 분석 구간에서 제외했다.

## 2. WPR을 끈 재비교

각 20초, seed 42는 기준→후보, seed 43은 후보→기준으로 교차 실행했다.
Object/Dispatcher 선택 프로파일은 모두 OFF다. 두 실행의 산술 평균이다.

| 항목 | 기준 | 후보 |
|---|---:|---:|
| 정상 FPS | 19.396 | 18.625 |
| CPU | 85.41% | 76.61% |
| 두 실행 중 최대 갱신 간격 | 155ms | 250ms |

후보는 CPU가 줄었지만 FPS도 약 3.97% 낮았다. 두 seed 모두 같은 방향이었다.
따라서 ‘WPR에서 회귀가 안 보였으니 이전 회귀는 없다’는 결론은 성립하지 않는다.
다만 WPR의 어떤 설정이나 OS 동작이 차이를 만들었는지까지 확인한 것은 아니다.

## 3. WPR 없이 Actor 일부만 표본 계측

기존 `ObjectSampleStride=256`, `DispatcherProfileStride=0`을 사용했다.
선택된 Actor만 타이머 dispatch·Actor 시작·Actor 본문 시간을 기록한다.
Dispatcher의 매 batch 시간 계측은 켜지 않았다. 소스와 EXE는 변경하지 않았다.

| seed / 모델 | FPS | CPU | 타이머 지연 평균 / P99 | Dispatcher 대기 평균 / P99 |
|---|---:|---:|---:|---:|
| 42 / 기준 | 19.440 | 85.72% | 18.22 / 63ms | 1.355 / 12ms |
| 42 / 후보 | 18.829 | 76.19% | 17.07 / 75ms | 0.795 / 4ms |
| 43 / 기준 | 19.446 | 86.66% | 17.20 / 59ms | 1.045 / 12ms |
| 43 / 후보 | 18.727 | 76.55% | 17.22 / 76ms | 0.880 / 5ms |

표본 실행에서도 두 seed 모두 회귀가 재현됐다. 후보의 Actor 본문 평균은 약 0.38~0.40µs로 기준과 비슷했다.
타이머 평균은 비슷하거나 낮아졌지만 P99는 나빠졌다. 평균만으로 보이지 않던 차이다.
P99는 1ms histogram 구간 값이며, 타이머와 Dispatcher P99를 더해 전체 P99로 취급하지 않는다.

측정 구간의 의미:

- 타이머 지연: Actor의 예정 deadline → Core Worker의 ObjectTimerTask dispatch.
  등록 요청 처리·wheel·poll·대기 종료 등의 지연을 포함한다. OS 대기만의 지연은 아니다.
- Dispatcher 대기: timer dispatch 직전 표본 시각 → Actor 실행 시작.
  실행기/Dispatcher 제출 비용과 큐 대기를 포함한다.

## 4. 대기 초과와 갱신 누락의 연결

다음은 실행 전체 누적값의 두 회차 평균이다. 타이머 지연 표본과 정상 FPS의 구간 경계가 정확히 같지는 않다.

| 항목 | 기준 | 후보 |
|---|---:|---:|
| 기한 초과 대기 복귀 횟수 | 2,830 | 6,044.5 |
| 기한 초과 누적 시간 | 22,061.5ms | 58,933.5ms |
| timed wait 초과 누적 시간 | 23,645.2ms | 62,231.5ms |
| Core signal wait 횟수 | 61,571.5 | 60,665 |
| 건너뛴 Actor tick | 826,698.5 | 1,641,542 |

대기 횟수 자체가 늘어난 것은 아니다. 초과 복귀 횟수와 누적 초과 시간이 늘었다.
누적 시간은 8개 Core Worker 합계이므로 한 번에 약 59초 멈췄다는 뜻이 아니다.

`Source/Core/Worker.cpp::wait_for_work()`는 예정 timer deadline보다 늦게 돌아오면
`m_sleepPastTimerCount/Ms`를 기록하고, timed wait의 예정 길이보다 오래 걸리면 overshoot를 기록한다.
이 시간에는 실제 OS 대기와 다시 CPU를 배정받기까지의 시간이 함께 들어갈 수 있다.

`Source/Examples/AreaUpdateExample/RuntimeBackend.h::update_object()`는 갱신 후 현재 시각으로 다음 예약을 계산한다.
`Source/Core/UpdateTickPlan.h`는 다음 50ms deadline까지 이미 지났으면 밀린 tick을 건너뛴다.
이번 후보의 타이머 P99가 75~76ms에 도달하면 이 누락이 늘 수 있고, 실제 skipped tick도 약 두 배였다.
이것은 누락 처리 코드의 버그라는 의미가 아니다. 밀린 갱신을 무제한 몰아 실행하지 않는 기존 정책이다.

## 판단과 다음 단계

후속 복귀 사유 계측을 완료했다. `WAIT-RETURN-RESULTS.md` 참고.
관측된 늦은 복귀는 대부분 timeout 판정이었으며, 후보의 timeout 복귀 빈도와 초과 누적 시간이 증가했다.

1. 28만에서 전달 Task 제거의 처리량 이득과 할당 비용 감소는 앞선 분석대로 유효하다.
2. 7만에서는 실행기 큐 지연은 개선됐지만 타이머 쪽 꼬리 지연 증가로 목표 갱신률이 낮아지는 현상을 확인했다.
3. 따라서 후보의 기본 채택은 계속 보류한다. 현재 실행 정책·기본 코드는 변경하지 않았다.
4. 다음 확인은 WPR 반복보다 `wait_until` 복귀를 timeout/신호로 구분하는 최소 계측이 우선이다.
   기존 시각을 재사용해 늦은 복귀가 어느 쪽에 몰리는지 확인하고, 대기 deadline 경로를 좁혀야 한다.

Windows 타이머 해상도 변화, 특정 mutex, CPU 캐시 경쟁 중 무엇이 회귀의 근본 원인인지는 확인 불가다.
가벼운 WPR에서 FPS가 바뀌었다는 사실만으로 WPR이 타이머 해상도를 바꿨다고 단정하지 않는다.
50ms 누락 정책은 유지하고 제시간에 처리하지 못하는 경로를 개선하는 것이 우선이다.

## 자료

- `artifacts/object-profiles-light-admin/analysis-summary.csv`: 경량 ETL 분석.
- 각 `export/object-analysis.json`, `object-functions.csv`, `object-thread-states.csv`: 상세.
- `unprofiled-control/results.csv`: WPR·선택 표본 OFF 대조 4회.
- `actor-sample-control/results.csv`: Actor 표본만 ON인 대조 4회.
- 모든 실행 폴더에 runtime.log·summary.json·CPU 표본 보존.
- `Run-ObjectNextComparison.ps1 -ObjectSampleStride 256`로 같은 유형의 표본 대조를 재현할 수 있다.
