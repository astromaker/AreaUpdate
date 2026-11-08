# AreaUpdate

큰 맵의 Actor 갱신을 Area 단위와 Actor 단위로 처리하고 비교하는 C++ 프로젝트다.
실제 게임 서버 전체가 아니라 공간 스케줄링·소유권·타이머와 합성 부하를 검증하는 실행 기반이다.

## 구현

| 구성 | 역할 | 예제 모드 |
|---|---|---|
| Core | Worker, Task, 큐, 타이머, 깨우기 | 공통 |
| AreaUpdate | 활성 Area와 공간 색상 파티션 스케줄링 | `area` |
| AreaUpdateLegacy | 최적화된 radius lock 실행기 | `legacy` |
| ObjectUpdate | Actor별 TaskExecuter와 round-robin Dispatcher | `job` |
| AreaUpdateLegacy/Baseline | 동결된 최초 비교 원본 | `original` |

`legacy`는 최적화된 예전 Area이며 동결 원본 `original`과 다르다.

## 빌드

Visual Studio C++ 도구와 Windows SDK를 설치한 뒤 저장소 루트의 PowerShell에서 실행한다.
아래 MSBuild 경로는 설치한 Visual Studio 버전에 맞춘다.

```powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
& $msbuild Source/AreaUpdate.sln /m:1 /nr:false /p:Configuration=Release /p:Platform=x64
```

Debug는 `Configuration=Debug`로 빌드한다. 출력은 `Source/x64/<구성>/`이다.
CMake 설정도 남아 있지만 이 저장소의 기본 빌드 검증은 솔루션과 MSBuild를 사용한다.

## 실행과 검증

```powershell
.\Source\x64\Release\AreaTests.exe
.\Source\x64\Release\UpdateComparison.exe --test
.\Source\x64\Release\AreaUpdateExample.exe 10 10000 286 42 1000 1 0 121 area
```

예제 인수는 순서대로 실행 초, Actor 수, 초당 이동 시도 수, seed, 입장 기간(ms),
입장 확인 간격(ms), Actor 추가 연산 횟수, 맵 한 변, 실행 모드다.
마지막 `area`를 `legacy` 또는 `job`으로 바꾸어 실행할 수 있다.
이 짧은 실행은 동작 확인용이며 정량 성능 비교가 아니다.

## 문서

- [구조와 안전 계약](docs/architecture.md)
- [성능 비교와 해석 범위](docs/performance.md)
- [설계 결정과 기각된 최적화](docs/decisions.md)
- [프로젝트 상세 구성](docs/project-structure.md)
- [Worker와 타이머](docs/runtime.md)
- [공간 범위 계산](docs/spatial-scheduling.md)
- [비교 시험 시나리오](docs/testing.md)
- [Object 프로파일링 절차](docs/profiling.md)
- [과거 실험 보고서와 재현 도구](docs/experiments/README.md)
- [개발·측정 도구 안내](tools/README.md)
- [코딩 규칙](CODING_STYLE.md), [저장소 작업 지침](AGENTS.md)

## 보관 원칙

소스·빌드 설정·검증 도구·현재 안내·재현 가치가 있는 실험 보고서는 Git에 포함한다.
원본 로그, CSV, ETL, 프로파일 내보내기, EXE/PDB와 실험 소스 복사본은 `artifacts/`에 보관하고
Git에서는 제외한다. 외부 보관 없이 이 폴더를 지우면 과거 실험의 완전한 재현이 불가능할 수 있다.
문서 정리는 커밋이나 원본 측정 자료 삭제를 자동으로 수행하지 않는다.
