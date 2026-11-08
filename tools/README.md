# 개발·측정 도구

## 구성

| 폴더 | 용도 |
|---|---|
| quality | 서식·명명·초기화·중괄호 검사 |
| benchmark | 현재 실행 파일 측정과 모델 비교 |
| validation | 이동·소유권·집계·도구 경로 검증 |
| profiling | 프로파일 수집·내보내기·계측 실행과 WPR 설정 |
| analysis | 기록된 결과의 분석·집계 |
| experiments | 과거 설정·기각한 후보·보관 바이너리 비교 |

benchmark의 도구도 각각 고유한 배치·반복·집계 조건을 가진다. 서로 다른 실행의 수치를
같은 시험으로 합치지 않는다. 상세 조건은 [실험 기록](../docs/experiments/README.md)을 참고한다.

## 경로 규칙

PowerShell 도구는 자신의 위치에서 두 단계 위를 저장소 루트로 계산한다.
상대 입력·출력 경로는 저장소 루트 기준이며, 스크립트 간 호출은 절대 경로로 해석한다.
Python 분석기의 상대 입력 경로도 저장소 루트 기준이고 절대 입력 경로는 그대로 사용한다.
문서의 명령은 저장소 루트에서 실행하는 표기다. 다른 폴더에서는 스크립트의 절대 경로로 호출한다.
도구를 다른 깊이로 다시 옮길 때는 루트 계산과 내부 참조도 함께 바꿔야 한다.

## 기본 검사

PowerShell 7을 사용한다. 서식·규칙 검사에는 LLVM 도구가, Python 분석에는 실제 Python 설치가 필요하다.
Windows의 앱 실행 별칭만 있고 Python이 설치되지 않은 경우 분석기 자체 테스트를 실행할 수 없다.

```powershell
.\tools\validation\Check-ToolPaths.ps1
.\tools\quality\Format-Code.ps1 -All -Check
.\tools\quality\Check-CodeStyle.ps1
.\tools\validation\Test-ScenarioAggregation.ps1
python .\tools\analysis\Analyze-RadiusEtw.py --self-test
python .\tools\analysis\Analyze-ObjectProfiles.py --self-test
```

경로 검사는 PowerShell 구문, 정적으로 명시된 도구 참조, 문서의 도구 경로와 WPR XML을 확인한다.
관리자 프로파일 수집이나 모든 부하 시나리오를 대신 실행하는 검사는 아니다.

## 측정과 원본 보관

솔루션을 Release x64로 빌드한 뒤 tools/benchmark/Measure-RuntimeCpu.ps1 또는
tools/benchmark/Run-CurrentModelComparison.ps1의 매개변수와 메모리 상한을 확인하고 실행한다.
프로파일 수집에는 관리자 권한과 Windows Performance Toolkit이 필요할 수 있다.
tools/experiments의 일부 도구와 Object 기준·후보 프로파일은 과거 실험 바이너리를 요구한다.
새 체크아웃에 없는 바이너리를 현재 실행 파일로 임의 대체하지 않는다.
기존 기록은 덮어쓰지 말고 새로운 출력 폴더를 지정한다.

결과·ETL·EXE/PDB·소스 복사본은 기존처럼 artifacts/에 두고 Git에서 제외한다.
도구 재배치는 스케줄링 정책·측정 인수·원본 결과를 바꾸지 않는다.
