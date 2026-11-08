# radius 모델 함수·스케줄링 추적 준비

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

후속 상태: 관리자 수집과 분석을 완료했다. 현재 결과는 `RADIUS-CPU-ANALYSIS.md`에 있으며 아래는 수집 준비 당시의 기록과 재현 방법이다.

## 현재 상태

실행 권한을 확장해 WPR CPU 추적을 시도했지만 `0xc5585011`로 시스템 프로파일 정책 활성화가 거부됐다. 현재 세션은 관리자 토큰이 아니므로 최신 실행의 함수별 CPU/스레드 추적은 아직 수집하지 못했다. 예전 partition Area 추적을 현재 radius 결과로 대신 사용하지 않는다.

Release 솔루션 빌드와 실행 파일/PDB 준비, PowerShell 구문 검사, 권한 사전 검사, 사용자 정의 WPR 프로파일 검증을 완료했다. 분석 내보내기는 기존 ETL로 시험했고 CPU 함수/스택, CSwitch 기반 스레드 CPU, ReadyThread, 대기 스택 파일 생성까지 확인했다. 이 시험 출력은 도구 검증용이며 최신 성능 근거가 아니다.

## 관리자 PowerShell에서 실행

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "D:\github\AreaUpdate\Collect-RadiusProfiles.ps1" -OutputDirectory artifacts/radius-profiles-admin
```

실행 정책 지정은 이 자식 프로세스에만 적용하며 시스템 정책을 영구 변경하지 않는다. 수집 중 별도 빌드나 부하 테스트를 함께 실행하지 않는다. 이미 같은 출력 폴더가 있으면 새 `-OutputDirectory`를 사용한다.

## 수집 내용

| 사례 | Actor / 맵 | 기록 | 목적 |
|---|---|---:|---|
| capacity | 280,000 / 640×640 | 10초 | 추가 진단 OFF에서 함수별 CPU와 호출 경로 확인 |
| hol | 280,000 / 640×640 | 10초 | 큐/HOL 진단·Area 표본과 스레드 실행 상태 교차 확인 |
| latency | 5,000 / 64×64 | 30초 | timer trace와 스레드 대기/실행 지연 확인 |

모두 현재 radius 모델(`legacy`)이며 기본 예약 정렬 ON이다. CPU는 6초 준비 후 기록한다. 전체 프로세스 실행 시간은 기록보다 길고, 저장 후처리 동안 대상이 끝날 수도 있다. 실행 파일/PDB 해시, 실행 설정, 대상 PID, 로그와 추적 정의를 함께 보관한다.

`tools/profiling/RadiusCpu.wprp`는 CPU 샘플·프로세스/이미지 정보·CSwitch·ReadyThread와 관련 스택만 선택한다. CPU 샘플과 대기 스택의 횟수를 같은 CPU 점유율로 합산하지 않는다. ETL의 유실 이벤트/버퍼와 심벌 해석 상태를 먼저 검증해야 한다.

## 이후 분석 순서

1. capacity의 대상 PID CPU 샘플에서 Area callback, radius 검사, 큐 처리, 알림과 atomic 경로를 구분한다. 자체 비용과 하위 호출 포함 비용을 구분한다.
2. hol의 관측 구간과 스레드가 실행·대기·Ready 상태인 구간을 비교한다. ETW만으로 radius mask가 실제 해제된 정확한 시각까지 보장되는 것은 아니며, 남는 범위는 확인 불가로 남긴다.
3. latency의 timer trace에서 휠 반올림, 대기 복귀, timer batch 앞선 callback 처리 시간을 구분하고 ETW 실행 상태와 맞춘다.

OS 심벌이 없는 프레임은 미해석으로 남긴다. 수집과 자동 내보내기는 `tools/profiling/Collect-RadiusProfiles.ps1`, 단일 사례의 재내보내기는 `tools/profiling/Export-RadiusProfile.ps1`에서 수행한다. 본 작업에서는 C++ 실행 로직을 수정하지 않았다.
