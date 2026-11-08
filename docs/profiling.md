# Object CPU 프로파일 수집

## 현재 상태

사용자가 관리자 PowerShell에서 수집한 8개 ETW 기록을 추출·분석했다.
결과는 `experiments/OBJECT-PROFILE-RESULTS.md`를 참고한다.
경량 기록 4개와 WPR 없는 대조 8회도 완료했으며, 후속 결과는 `experiments/OBJECT-LIGHT-PROFILE-RESULTS.md`에 정리했다.
`artifacts/object-profile-preflight`의 사전 실행은 실제 CPU 기록과 구분한다.

## 대상

- 기준: `artifacts/object-next/baseline`.
- 전달 Task 제거 후보: `artifacts/object-next/direct`.
- 최신 빌드로 대체하지 않고, 실제 성능 비교에 쓴 보관 EXE/PDB를 그대로 사용한다.
- EXE 해시를 기존 비교 CSV와 대조하고, EXE/PDB GUID와 Age도 비교한다.
- 7만·28만 Actor, seed 42/43, 총 8개 기록. 각 회차와 Actor 단계에서 실행 순서를 바꾼다.
- 맵 320/640, 이동 시도 초당 2,000/8,000, 1초/1ms 분산 입장, 추가 연산 0.
- Object 모드, signal, 고정 주기, Task 통계 ON, 상세 timer/Object/Dispatcher 표본 추적 OFF.
- 대상은 기본 30초 실행. 시작 6초 후 CPU 프로파일을 10초 요청한다.
  실제 ETL 기록 범위는 시작/중단 호출과 병합의 영향을 포함하므로 요청한 10초와 동일하다고 가정하지 않는다.
- 기존 검증된 `tools/profiling/RadiusCpu.wprp`의 CPU sampling·CSwitch·ReadyThread 스택 설정을 재사용한다.
- 대상 private 메모리 1,500MiB 상한과 종료 감시를 적용한다.
- 자기 기록의 고유 instance만 중단·취소하며 다른 WPR 기록을 강제 취소하지 않는다.

## 관리자 PowerShell에서 실행

아래 명령은 이 실행에만 스크립트 정책을 적용하며 시스템 실행 정책을 영구 변경하지 않는다.
기존 출력 폴더가 있으면 덮어쓰지 않으므로 새로운 경로를 지정해야 한다.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\github\AreaUpdate\Collect-ObjectProfiles.ps1 -OutputDirectory artifacts/object-profiles-admin
```

수집 중 다른 성능 시험·빌드·게임은 실행하지 않는 것이 좋다.
8개 대상 실행과 WPR 병합이 순차 진행되므로 수 분이 걸린다.

## 수집 후 분석 계획

`tools/profiling/Export-ObjectProfiles.ps1`로 각 ETL의 CPU 함수/스택, thread CPU, CSwitch/ReadyThread, 대기 스택과 유실 통계를 추출한다.
기존 OS 심볼 캐시가 있으면 사용한다. 캐시에 없는 OS 심볼은 미해결로 남을 수 있으며 필요시 별도로 확보한다.
EXE/PDB는 각 기록의 `bin` 폴더를 최우선 사용한다.

비교 항목:

1. Task 생성·소멸, 힙 할당/해제, shared_ptr 수명 관리 경로의 샘플.
2. 실행기/Dispatcher/타이머 Worker와 시각 조회 경로의 샘플.
3. 기준과 후보의 실제 실행·Ready·Waiting 비율 및 스레드 전환.
4. 7만에서 후보가 느려지고 28만에서 빨라진 패턴이 두 기록에서도 반복되는지.

CPU own/exclusive와 inclusive 샘플을 구분하고 같은 샘플을 중복 합산하지 않는다.
atomic 명령 주변의 샘플만으로 캐시 일관성 비용을 단정하지 않는다.
Ready/Waiting 벽시계는 CPU 사용 시간과 분리하며, 프로파일 실행 FPS를 비계측 성능 표와 혼합하지 않는다.
이벤트 유실·미해결 심볼·기록 경계를 확인하기 전에는 정밀 비율을 결론 내리지 않는다.

`tools/profiling/Export-RadiusProfile.ps1`에는 선택적인 심볼 경로 인수만 추가했다. 기존 호출의 기본 동작은 동일하다.
C++와 기본 스케줄링 정책은 수정하지 않았다.

## 7만 가벼운 재수집

무거운 기록에서는 7만의 스택 수집 비용이 커 성능 회귀의 원인을 확정하지 못했다.
`tools/profiling/ObjectCpuLight.wprp`는 CPU 샘플 스택과 CSwitch/ReadyThread 이벤트를 유지하되,
CSwitch/ReadyThread마다 수집하던 스택만 제외한다. 기록 비용이 0이 되는 것은 아니다.
WPR 프로파일 목록 검사와 사전 실행으로 설정을 확인했다.

관리자 PowerShell에서 다음 한 명령으로 7만의 기준/후보를 두 회차 수집한다.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\github\AreaUpdate\Collect-ObjectProfiles.ps1 -LightScheduling -ActorCounts 70000 -OutputDirectory artifacts/object-profiles-light-admin
```
