# 이동·상호작용 검증과 합성 부하 비교

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

## 결론 범위

실제 게임 로직은 제공되지 않았다. 따라서 실제 게임 서버 성능은 확인 불가다.
이 문서는 이동 중 이벤트 전달의 정확성과 별도의 합성 부하 결과를 구분한다.
CPU/힙 프로파일 수집은 현재 실행 권한으로 거부돼 완료하지 못했다.
이전 선택형 계측 수치를 실제 CPU 호출 스택 프로파일 결과라고 대체하지 않는다.

## 1. 정확성 검증

MotionInteractionProbe는 기존 Area/Worker/RuntimeBackend와 Actor를 사용하며 프로덕션 소유권 코드는 바꾸지 않는다.
Actor는 Area 이동 중에도 ID 기반 inbox로 이벤트를 받고 자신의 update에서 적용한다.
이것은 명시적으로 선택한 테스트 전달 의미이며, 이동 중 대상에 잠금 없이 직접 쓰는 모든 코드를 보장하지 않는다.

- 균일/한 지점 집중 배치, 5,000 Actor, seed 42/43, Area/Object 각 실행
- controller가 이동 요청과 외부 피해 이벤트를 동시에 제출
- 수신자별 기대 이벤트 ID와 실제 적용 ID multiset 대조로 유실·중복·잘못된 대상 적용 검출
- 수신자별 피해 합계와 전체 피해 합계 검증, 동시 Actor update 감지
- 이동 중단 후 마지막 수락 목적지에서 두 번 이상 update했는지 확인
- Area 모드는 전원 탈퇴→모든 Area inactive→update 중단 확인→탈퇴 중 이벤트 접수→재등록·적용→재탈퇴 검증
- 종료 후 Task 생성/소멸 검증과 tasks_live=0 확인

탈퇴해도 Actor 메모리는 유지한다. Actor 파괴/ID 재사용 중 이벤트 전달까지 검증한 것은 아니다.
Object는 Area 목록 소유 모델이 아니므로 Area 탈퇴/재등록 검사는 해당 없음이다.
관측 목적지는 quiescence 뒤의 update 문맥으로 확인하며 private ownership 필드를 무잠금으로 읽지 않는다.

검증기 음성 테스트는 ID 하나를 의도적으로 손상시킨다. 전체 이벤트 수/피해 합계가 같더라도
수신자별 ID 대조에서 실패해야 성공으로 처리한다. 정상 종료 또는 다른 원인의 실패는 테스트 실패다.
검증 모드의 FPS/CPU는 로그에 있지만 성능 결론으로 사용하지 않는다.

최종 검증은 8회 모두 성공했다. 총 938,912건의 이벤트 적용을 대조했고,
Area 4회에서는 각각 탈퇴/재등록 주기까지 통과했다. 의도적 ID 손상은
`Event missing, duplicate or misdirected` 오류로 거부돼 음성 테스트도 통과했다.
원자료는 artifacts/motion-checks-clean/verify에 있다.

## 2. 합성 부하 정의

실제 로직 대신 다음의 간단한 모델을 별도 synthetic 모드에 넣었다.
- 실제 Area 이동 또는 Object 위치 이동
- Actor별 100ms 공격 쿨타임
- 최대 16개 임의 후보의 마지막 관측 위치를 읽고 Chebyshev 거리 2 이내인지 판단
- 유효 후보에 inbox 이벤트 전송, 피해 누적과 단순 체력 갱신

이는 완전한 AOI 검색·경로 탐색·네트워크·DB·실제 전투 규칙이 아니다.
사거리 판단은 위치 snapshot 기준이며 수신 때의 위치 재검사 의미는 포함하지 않는다.
64×64 맵에서 Actor를 먼저 생성·등록하고 Worker를 시작한다. 기존 대형 맵/분산 입장 시험과 조건이 다르다.
같은 Actor payload와 초기 seed를 쓰지만 이동 수락 시점과 update 타이밍에 따라 실제 이동/공격 수는 달라진다.
따라서 CPU 비율을 동일 연산량의 단순 배속으로 해석하지 않는다.

각 조건 15초씩 두 seed를 사용하고 두 번째 seed에서는 실행 순서를 뒤집었다.
FPS는 첫 1초를 제외한 update 카운터 snapshot 차이, CPU는 15초 루프 전체 프로세스 시간이다.
CPU 시간은 기존 ComparisonTiming의 프로세스 CPU API를 사용한다. MSVC std::clock은 사용하지 않는다.
무거운 ID ledger·정렬 대조는 성능 실행에서 끄지만 inbox 동기화·기본 카운터·Task 통계 ON은 유지한다.
최종 count/damage 합계와 수명 검사·drain은 시간 측정 후 수행한다.
이 합성 fixture는 p99 갱신 간격을 측정하지 않으므로 평균 FPS가 deadline 준수를 뜻하지 않는다.

Object 경로에도 Area wake callback을 연결했던 초기 fixture를 수정했다.
Object 실행에서 AreaExecuter.readyWork가 0인지 검사한다. 수정 전 결과는 참고용으로만 보존하고
최종 성능 결론은 수정 후 같은 실행 파일로 다시 비교한 결과를 사용한다.

최종 재측정은 조건별 2회이며 아래는 실행별 중앙값(2회 평균)이다.

| Actor / 배치 | Area 안정 FPS | Object 안정 FPS | Area CPU | Object CPU |
|---|---:|---:|---:|---:|
| 5,000 / 균일 | 19.58 | 20.00 | 1.80% | 14.34% |
| 5,000 / 집중 | 19.49 | 20.04 | 0.83% | 13.60% |
| 50,000 / 균일 | 19.97 | 19.84 | 7.96% | 70.22% |
| 50,000 / 집중 | 19.90 | 19.89 | 4.15% | 68.56% |

Area는 이 모델에서 CPU가 낮았지만, 5,000 Actor의 높은 이동 빈도에서 평균 갱신은 Object보다 조금 낮았다.
이동 수락 수와 공격 수는 로그에 별도 기록한다. 초기 배치가 같아도 행동 결과가 완전히 같은 입력 재생은 아니다.
CPU 차이에 할당/Task 수명 통계 비용도 포함된다. Task 통계 OFF 대조는 이 합성 모델에서 수행하지 않았다.
본 표로 실제 게임의 비용이나 일반적인 배속을 확정하지 않는다.
원자료는 artifacts/motion-checks-clean/synthetic이며 수정 전 motion-checks 결과와 섞지 않았다.
피해 처리뿐 아니라 최종 체력 체크섬도 출력해 체력 상태 계산이 관측되도록 했다.

## 3. 프로파일 수집 실패와 정리

설치된 WPR/xperf/WPA와 Visual Studio 진단 도구를 확인했다.
CPU 수집은 WPR CPU.Verbose.File 시작에서 0xc5585011로 실패했다.
현재 토큰에는 SeSystemProfilePrivilege가 없었다. require_escalated 실행도 Windows 관리자 토큰을 제공하지 않았다.
힙 설정은 0x80070005 접근 거부, Visual Studio 진단 서비스도 접근 거부로 시작하지 못했다.
ETL 데이터가 없으므로 함수별 CPU 비중/할당 스택/누수 원인에 대한 프로파일 분석 결과는 없다.

실패 후 확인한 상태:
- WPR recording 없음, 고유 시험 세션도 recording 없음
- AreaHeapProbe.exe heap tracing disabled
- 이 작업에서 시작한 AreaCpuProbe/AreaHeapProbe 프로세스 없음

Collect-AreaProfiles.ps1은 별도 복사 실행 파일과 고유 WPR 인스턴스를 사용한다.
기존 힙 설정이 enabled라면 덮어쓰지 않고, 새 설정은 finally에서 해제하도록 했다.
실패 시에도 다른 사용자의 기록을 전역 취소하지 않는다.
CPU는 28만 Actor의 준비 후 10초, 힙은 5,000 Actor의 시작 3초를 수집하도록 제한했다.
힙 기록은 프로세스 생존 중 끝나므로 성공하면 종료 전 outstanding 할당을 볼 수 있다.
추적은 오버헤드가 있으므로 그 실행의 FPS를 일반 성능과 섞지 않는다.

관리자 권한/허용된 시스템 정책을 가진 PowerShell에서 새 폴더로 재시도한다.
```powershell
.\Collect-AreaProfiles.ps1 -OutputDirectory artifacts/motion-profiles-admin
.\Export-AreaProfiles.ps1 -InputDirectory artifacts/motion-profiles-admin
```
내보내기 스크립트는 로컬 PDB만 사용하며 CPU 스택과 해당 PID의 힙 할당 스택/손실 통계를 출력한다.
당시에는 ETL 수집 실패로 내보내기 경로가 미검증이었다. 이후 관리자 수집이 성공했으며 실제 CPU·힙 분석 결과는 [후속 보고서](AREA-ETW-PROFILE-RESULTS.md)에 정리했다.

## 재현

```powershell
.\Run-MotionChecks.ps1 -Stage verify -OutputDirectory artifacts/motion-repeat
.\Run-MotionChecks.ps1 -Stage synthetic -OutputDirectory artifacts/motion-repeat
```

프로덕션 파티션 정책/반경/Actor 소유권/Job round-robin과 오브젝트 풀은 변경하지 않았다.
추가 CMake 테스트는 MovingActorEvents이며 실제 빌드 검증은 기존 지침대로 솔루션 MSBuild를 사용했다.
