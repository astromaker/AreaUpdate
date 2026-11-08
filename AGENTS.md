# 저장소 작업 지침

- 답변과 사용자용 문서는 한국어로 작성한다.
- 코드 스타일은 CODING_STYLE.md와 .clang-format을 따른다. 공백 4칸, CRLF, C++ 파일은 기존 UTF-8 BOM을 유지한다.
- 본문이 물리적으로 한 줄인 if/반복문만 중괄호를 생략한다. 중첩 제어문·여러 줄 호출·여러 문장·잠금 scope에는 중괄호를 넣는다. 바깥 for의 본문이 중첩 for 하나여도 여러 줄이면 중괄호가 필요하다.
- bool 부정 조건은 == false, 포인터 비교는 nullptr를 명시한다. 이 규칙은 formatter가 대신해 주지 않는다.
- 클래스는 PascalCase, 함수는 snake_case, 비공개/보호 멤버는 m_camelBack을 쓴다. 공개 데이터 구조체는 camelBack, 내부 상태 구조체는 올바른 m_camelBack도 허용한다. 단순 getter 외의 함수는 한 줄로 축약하지 않는다.
- Source/AreaUpdateLegacy/Baseline, Source/AreaUpdateLegacy/OriginalAreaAdapter.*, ThirdParty는 포맷·명명 변경에서 제외한다.
- 일괄 포맷은 명시적으로 요청받은 범위에만 적용한다. 확인 없이 전체 소스를 재포맷하지 않는다.
- 빌드 검증은 Source/AreaUpdate.sln을 MSBuild로 수행한다. Release/Debug 구성을 변경 범위에 맞게 검증한다.
- 공간 범위, batch 64, partition 공평성, Actor 소유권, Job round-robin 재전달 정책을 서식 수정과 함께 바꾸지 않는다.
- 오브젝트 풀과 idle Worker 관리/stealing을 요청 없이 추가하지 않는다.
- 기존 작업 트리의 사용자 변경을 되돌리지 않는다.
- 변수는 항상 초기화한다. 포인터는 nullptr, 구조체는 {}를 사용하고 수치·bool은 의미 있는 초기값을 지정한다. 멤버는 선언부 또는 생성자 초기화 목록에서 초기화한다.
- 함수 내부의 논리적 단계 사이에는 빈 줄 하나를 넣는다. 준비·제출·대기·종료·검증을 구분하되 같은 목적의 연속 대입/검증은 묶는다. formatter가 대신 판단하지 않으므로 작성 시 직접 적용한다.
- 전체 규칙 적용 후 Format-Code.ps1 -All -Check와 Check-CodeStyle.ps1로 서식·명명·초기화·중괄호를 확인한다. 기존 프로젝트의 C++ 언어 설정을 스타일 변경을 위해 올리지 않는다.
