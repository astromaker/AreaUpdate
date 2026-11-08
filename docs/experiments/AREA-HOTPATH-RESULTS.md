# Area 갱신 경로 최적화 비교

> 보관된 실험 기록입니다. 본문의 현재·기본값은 측정 당시를 가리킵니다.
> 현행 상태는 [성능 요약](../performance.md)과 [설계 결정](../decisions.md)을 확인하세요.
> 명령어와 artifacts/ 경로는 저장소 루트를 기준으로 합니다.

## 결론

세 후보를 순서대로 독립 비교했고, `Area::update_at()` 경로 정리만 채택했다.
시간 조회 중복 제거와 radius mask 저장 구조 변경은 일관된 이득이 없어 제거했다.
공간 범위, 잠금 순서, FIFO 선두 충돌 정책, batch 64, Actor 소유권과 이동 중 갱신 보장은 변경하지 않았다.

## 측정 조건

- `Source/AreaUpdate.sln`, MSBuild Release x64, 논리 프로세서 8개.
- 예전 Area의 최적화 radius 모델(`legacy`), signal 대기, 고정 주기 예약, 5ms × 256 타이머 휠.
- 맵 640 × 640, Actor 280,000명, 이동 요청 초당 8,000회.
- 입장 1,000ms 동안 1ms 단위 분산, 실행마다 20초.
- seed 42/43 각각 2회. 기준/후보 순서를 교차한 네 쌍의 비교.
- 70,000명은 seed 42로 기준/후보 각 1회 확인.
- Actor 추가 연산 0인 기존 합성 부하. 실제 게임 로직 성능을 의미하지 않는다.
- task 통계 활성화, radius CPU/queue 진단 비활성화, timer trace 임계값 20ms.
- 측정 중 다른 빌드나 무거운 검사는 실행하지 않았다.
- CPU는 전체 논리 프로세서 용량을 100%로 정규화한 정상 구간 값이다.

## 280,000명 결과

FPS·CPU·최저 Actor FPS는 네 실행의 산술 평균이다. 최대 간격은 네 실행 중 최댓값이다.
각 단계 안의 기준/후보만 비교해야 한다. 단계 사이 기준 수치 차이는 별도 실행의 변동을 포함한다.

| 후보 | 기준 FPS → 후보 FPS | 변화 | CPU % | 최저 Actor FPS | 최대 갱신 간격 | 결정 |
|---|---:|---:|---:|---:|---:|---|
| 재예약 시각 조회 2회 → 1회 | 14.80 → 14.54 | -1.7% | 81.80 → 78.41 | 13.74 → 13.42 | 244 → 347ms | 제외 |
| Area 갱신 일반 경로 정리 | 15.33 → 15.82 | +3.2% | 84.11 → 85.12 | 14.37 → 14.80 | 238 → 147ms | 채택 |
| 불변 mask를 vector 대신 배열+길이로 저장 | 16.11 → 16.31 | +1.2% | 86.47 → 86.07 | 15.00 → 15.18 | 151 → 244ms | 제외 |

Area 경로 후보는 네 쌍 모두 FPS가 상승했다. 최대 간격은 세 쌍에서 감소했지만 한 쌍에서는 138 → 147ms로 증가했다.
따라서 처리량의 소폭 개선으로 판단하며 최악 지연 제거로 해석하지 않는다.
mask 후보는 두 쌍 상승·두 쌍 하락이었고, 하락한 두 쌍은 최대 간격도 악화되어 제외했다.
유의성 검정이나 장시간 안정성 시험은 이번 비교에 포함되지 않았다.

70,000명에서 채택 후보는 20.00 → 20.01FPS, CPU 36.32 → 38.35%, 최대 간격 114 → 107ms였다.
이 부하에서는 CPU 절감 효과를 확인하지 못했다. 모든 부하에서 3.2% 개선된다는 뜻은 아니다.

## 남긴 코드

`Source/AreaUpdate/Area.cpp`:

1. pending 등록·탈퇴 반영 후 resident Actor가 있으면 두 번째 활성 상태 조회와 탈퇴자 검사 경로를 건너뛴다.
   resident 제거는 해당 Area의 update에서만 일어나므로, 비어 있지 않은 목록은 object count 기반 활성을 보장한다.
2. resident가 없으면 기존처럼 수동 활성 상태와 이동 중 Actor를 확인한다.
   마지막 Actor의 단순 탈퇴는 불필요한 `do_update()`를 호출하지 않고, 이동 중 overdue Actor 갱신은 유지한다.
3. 가장 이른 Actor deadline을 지역 변수에서 누적하고, resident 순회 후 `m_nextActorTick`에 한 번 반영한다.
   다음 Area 예약 계산은 기존 계약 그대로 이 값을 사용한다.

`RuntimeBackend.h`의 시각 조회와 `RadiusExecuter.*`의 mask 저장은 실험 전 구현을 유지한다.
Worker·ObjectUpdate·공평성 정책은 수정하지 않았다. Area 공통 코드를 사용하므로 새 Area에도 경로 정리가 적용되지만,
위 처리량 수치는 예전 Area 모델만 측정한 값이다.

## 회귀 테스트

- AreaTests: 비활성 skip, 수동 활성 후 비활성, Actor 보유 중 활성 유지, 서로 다른 deadline의 최솟값, 마지막 Actor 탈퇴.
- footprint 1/10/11/40개 word 및 재설정: 충돌, 부분 획득 롤백, 해제, 빈 footprint 거부 후 기존 설정 유지.
- 기존 radius2 대각선 범위·동시 실행·예외 해제 테스트 유지.
- handoff-chain: 1초 동안 100번 이동, 이동 Actor 20회 갱신, 같은 시각 중복 갱신 없음, 탈퇴 후 재등록 확인.
- 각 후보의 legacy uniform/boundary 이동·상호작용 검증 통과.
- 비교 실행에서 neverUpdated=0, processExitCode=0, tasksLive=0 확인.
- 최종 채택 코드: 솔루션 Release/Debug x64 빌드와 양쪽 AreaTests 통과.
- 최종 Release의 area/legacy × uniform/boundary 검증: overlaps=0, sent=applied, tasks_live=0.
- `Format-Code.ps1 -All -Check` 33개 파일 및 `Check-CodeStyle.ps1` 전체 검사 통과.
- 변경 C++ 파일의 UTF-8 BOM·CRLF 보존, `git diff --check` 통과.

## 재현 자료

- `Run-HotpathComparison.ps1`: 두 실행 파일 묶음을 교차 실행하고 결과 CSV 저장.
- `artifacts/area-hotpath/{clock,path,mask}-comparison/results.csv`: 원시 결과와 CPU·공평성·지연 지표.
- 각 비교 폴더의 `binaries.csv`: 실행 파일 SHA256.
- `artifacts/area-hotpath/reference`: 실험 전 관련 소스.
- `baseline`, `clock`, `area-path`, `masks`: 각 단계의 실행 파일과 PDB.
- `masks`와 `clock` 실행 파일은 제외된 실험 후보이며 최종 코드가 아니다.

예시: 기준/채택 후보를 다시 비교하려면 새 출력 경로를 지정한다.

```powershell
.\Run-HotpathComparison.ps1 -BaselineDirectory artifacts/area-hotpath/baseline -CandidateDirectory artifacts/area-hotpath/area-path -OutputDirectory artifacts/area-hotpath/path-recheck
```
