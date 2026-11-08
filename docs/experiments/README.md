# 실험 기록과 재현 도구

이 폴더는 과거 결과의 근거를 보존한다. 현재 사용법은 [루트 안내](../../README.md),
최종 비교는 [성능 요약](../performance.md), 채택 여부는 [설계 결정](../decisions.md)부터 읽는다.
보고서의 현재·기본값·후속 과제는 측정 당시를 뜻하며 현행 Source와 다를 수 있다.

## 우선 읽을 기록

- [최신 세 모델 비교](CURRENT-MODEL-COMPARISON.md)와 [42만·56만 부하](CURRENT-MODEL-HIGH-SCALE.md)
- [개선 Area 갱신 경로](AREA-HOTPATH-RESULTS.md)
- [예전 Area 안정성과 최악 지연](RADIUS-STABILITY-RESULTS.md)
- [이동 중 Actor 갱신 예정 유지](ACTOR-DEADLINE-RESULTS.md)
- [Object CPU 분석](OBJECT-CPU-ANALYSIS.md)과 [추가 후보 채택 판단](OBJECT-NEXT-OPTIMIZATION.md)
- [대기 복귀 계측](WAIT-RETURN-RESULTS.md)과 [기각된 능동 대기 실험](ACTIVE-WAIT-RESULTS.md)

## 도구와 원본 자료

재현 스크립트는 저장소 루트에 유지했다. 문서 위치와 무관하게 모든 명령은 저장소 루트에서 실행한다.
옛 보고서는 당시의 옵션과 실행 파일을 전제로 하므로 현행 바이너리에 그대로 적용할 수 있다고 가정하지 않는다.

| 용도 | 주요 도구 |
|---|---|
| 세 모델 비교 | Run-CurrentModelComparison.ps1, Measure-RuntimeCpu.ps1 |
| 통제된 상호작용 비교 | Run-ScenarioComparison.ps1, Test-ScenarioAggregation.ps1 |
| 이동·소유권·공간 검증 | Run-MotionChecks.ps1, Run-ActorDeadlineValidation.ps1 |
| Area 프로파일 | Collect-AreaProfiles.ps1, Export-AreaProfiles.ps1, Analyze-AreaProfile.ps1 |
| 예전 Area 프로파일 | Collect-RadiusProfiles.ps1, Export-RadiusProfile.ps1, Analyze-RadiusEtw.py |
| Object 프로파일 | Collect-ObjectProfiles.ps1, Export-ObjectProfiles.ps1, Analyze-ObjectProfiles.py |
| 기각한 후보 재현 | Run-ObjectNextComparison.ps1, Run-ActiveWaitComparison.ps1 |

특히 능동 대기 등 기각한 후보의 재현은 artifacts/ 아래 보관된 실험 EXE·소스가 필요하다.
보고서와 스크립트만 받은 새 체크아웃에서는 해당 바이너리가 없어 재현되지 않을 수 있다.
현재 소스를 빌드하는 검증과 옛 바이너리를 실행하는 비교를 구분한다.

## Git 보관 원칙

보고서와 재현 도구는 Git에 포함하되, 실험 EXE/PDB·ETL·CSV·원본 로그·임시 소스 복사본은
artifacts/에 보관하고 Git에서는 제외한다. 배포할 필요가 있는 측정 원본은 별도 보관소로 전달한다.
수치·한계·채택 판단을 남기기 위해 이번 정리에서는 보고서를 삭제하지 않았다.

## 전체 보고서

아래 목록은 보관한 보고서의 제목과 파일을 기준으로 생성했다.

- [짧은 기한 능동 대기 실험](ACTIVE-WAIT-RESULTS.md)
- [Actor deadline 수정 후 검증과 대기 분해](ACTOR-DEADLINE-FOLLOWUP.md)
- [이동 중 Actor 갱신 시각 유지](ACTOR-DEADLINE-RESULTS.md)
- [Actor 수 증가 테스트](ACTOR-SCALE-RESULTS.md)
- [Actor 실행 Task 분리와 제출 대기 표본 측정](ACTOR-TASK-RESULTS.md)
- [시간 제한 적응형 파티션 입장](AREA-ADAPTIVE-RESULTS.md)
- [1단계: 전역 Area 실행 카운터 영향](AREA-COUNTER-COMPARISON.md)
- [Area CPU·힙 추적 결과](AREA-ETW-PROFILE-RESULTS.md)
- [Area 고정 주기 예약과 Object 비교](AREA-FIXED-COMPARISON.md)
- [Area 후속 최적화: 파티션 캐싱과 예약 묶음](AREA-FOLLOWUP-COMPARISON.md)
- [Area 갱신 경로 최적화 비교](AREA-HOTPATH-RESULTS.md)
- [Area 실행 경로 최적화](AREA-OPTIMIZATION-RESULTS.md)
- [Area 병목 계측](AREA-PROFILE-RESULTS.md)
- [Area 큐 지연 분해와 파티션 처리량](AREA-QUEUE-PROFILE-RESULTS.md)
- [Area 후속 최적화와 검증](AREA-VALIDATION-RESULTS.md)
- [Area CPU 활용 부족: admission barrier와 폴링 대기](AREA-WAIT-DIAGNOSIS.md)
- [20 FPS 미달과 CPU 한계 계측](CAPACITY-RESULTS.md)
- [Actor Task와 Area 비교](COMPARISON.md)
- [최신 Area / Area(old) / Object 성능 비교](CURRENT-MODEL-COMPARISON.md)
- [최신 3모델 고부하 비교: 42만·56만 Actor](CURRENT-MODEL-HIGH-SCALE.md)
- [실제 Dispatcher Worker 부하 분산 확인](DISPATCHER-BALANCE-RESULTS.md)
- [Object 고정 주기 예약 비교](FIXED-PERIOD-RESULTS.md)
- [Job Worker 부하 불균형 확인](JOB-LOAD-BALANCE.md)
- [예전 Area 최적화와 새 Area 비교](LEGACY-OPTIMIZATION-RESULTS.md)
- [이동·상호작용 검증과 합성 부하 비교](MOTION-GAME-PROFILE-RESULTS.md)
- [Object 모델의 높은 CPU 사용 원인](OBJECT-CPU-ANALYSIS.md)
- [7만 Object 경량 프로파일·대조 측정](OBJECT-LIGHT-PROFILE-RESULTS.md)
- [Object 추가 최적화 4단계 비교](OBJECT-NEXT-OPTIMIZATION.md)
- [ObjectUpdate 공통 비용 개선](OBJECT-OVERHEAD-RESULTS.md)
- [Object 기준·전달 Task 제거 후보 프로파일 결과](OBJECT-PROFILE-RESULTS.md)
- [최적화 및 3방식 비교](OPTIMIZATION.md)
- [초기 AreaUpdate 측정 기록](PERFORMANCE.md)
- [현재 radius 모델 CPU·HOL·타이머 분석](RADIUS-CPU-ANALYSIS.md)
- [예전 Area 동일 bucket 및 알림 통합 비교](RADIUS-FAST-RESULTS.md)
- [예전 Area 후속 최적화](RADIUS-FOLLOWUP-RESULTS.md)
- [남은 Actor 갱신 지연 원인](RADIUS-LATENCY-CAUSES.md)
- [radius 모델 함수·스케줄링 추적 준비](RADIUS-PROFILING-NEXT.md)
- [예전 Area 묶음 게시 최적화](RADIUS-PUBLICATION-RESULTS.md)
- [예전 Area 안정성과 최악 지연 검증](RADIUS-STABILITY-RESULTS.md)
- [동작 유지 리팩토링](REFACTORING.md)
- [분산 입장 런타임 3종 비교](RUNTIME-THREE-WAY-RESULTS.md)
- [Signal 기본 대기 적용 후 Area와 Object 비교](SIGNAL-MODEL-COMPARISON.md)
- [Worker sleep_for 폴링 실험](SLEEP-RESULTS.md)
- [표준 C++ 전환 측정 결과](STD-PORT-RESULTS.md)
- [Area / Actor Job 비교 결과](TEST-RESULTS.md)
- [Area 원본 / Area 수정본 / Actor Job 결과](THREE-WAY-RESULTS.md)
- [공평성 입장 제한 제거 실험](UNLIMITED-ADMISSION-RESULTS.md)
- [Object 타이머 Worker 복귀 지연 확인](WAIT-RETURN-RESULTS.md)
- [Worker 깨우기와 sleep 비교](WAKEUP-RESULTS.md)
- [256슬롯 타이머 휠 1ms 비교와 지연 추적](WHEEL-RESOLUTION-RESULTS.md)
- [첫 실행 시도 전 대기 계측과 Worker 처리 비율 실험](WORKER-DISPATCH-RESULTS.md)
