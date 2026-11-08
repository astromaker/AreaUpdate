# 코딩 규칙

## 서식

- 변수는 선언 시 초기화한다. 포인터는 `nullptr`, 수치·bool은 의미 있는 초기값, 구조체는 `{}`로 초기화한다. 생성자 초기화 목록으로 초기화하는 멤버는 유효하다. `cppcoreguidelines-init-variables`로 지원되는 지역 변수 초기화를 검사하고 구조체·멤버의 누락은 리뷰로 보완한다.

- 공백 4칸, 최대 120자, CRLF. 현재 C++ 파일의 UTF-8 BOM을 유지한다.
- 클래스는 PascalCase, 함수와 메서드는 snake_case, 지역 변수와 매개변수는 camelBack.
- 비공개·보호 멤버는 `m_actorCount` 형태, 클래스/전역 상수는 UPPER_CASE. 공개 데이터 구조체는 camelBack을 사용하며 내부 상태 구조체의 올바른 `m_camelBack` 표기도 허용한다.
- 포인터와 참조는 타입에 붙인다: `Actor* actor`, `const Area& area`.
- 중괄호는 다음 줄. 본문이 물리적으로 한 줄인 if/반복문만 중괄호를 생략하고 본문은 다음 줄에 쓴다.
- 중첩 제어문이나 여러 줄로 나뉜 함수 호출도 본문이 여러 줄이면 중괄호를 반드시 넣는다. 문법상 한 문장이라는 이유로 생략하지 않는다.
- 여러 문장, 명시적인 잠금 수명, 의미 있는 별도 scope의 중괄호는 유지한다.
- 빈 람다는 `[] {}` 한 줄로 쓴다. 내용이 있는 람다는 여러 줄로 작성한다.
- 단순 getter 외의 함수는 여러 줄로 작성한다. formatter는 getter를 의미적으로 구분하지 못하므로 자동 적용 시 getter도 여러 줄로 전개한다.
- bool 부정 조건은 `is_active() == false`, 포인터는 `actor == nullptr`처럼 명시한다. 이는 formatter가 강제하지 못하는 작성 규칙이다.

```cpp
if (is_active() == false)
    return;

{
    std::lock_guard<std::mutex> guard(m_pendingLock);
    m_pendingAdds.push_back(actor);
    m_pending.store(true);
}

notify_scheduler();
```

## 자동 적용

함수 안에서는 준비·작업 제출·완료 대기·종료·결과 검증처럼 논리적 단계 사이에 빈 줄 하나를 둔다. 같은 목적의 연속 대입이나 검증은 붙여 쓴다. formatter는 논리적 경계를 판단하지 못하므로 작성과 리뷰에서 나누고, 자동 포맷은 그 구분을 유지하도록 사용한다.

LLVM clang-format 18 이상이 필요하며 현재 LLVM 22에서 설정을 검증했다. 버전에 따라 서식 결과가 달라질 수 있으므로 팀에서는 같은 버전을 사용한다. PowerShell 7에서 실행한다.

```powershell
# 기본: Area.cpp 하나만 적용
.\tools\quality\Format-Code.ps1
# 선택 파일을 수정 없이 검사
.\tools\quality\Format-Code.ps1 -Check -Paths Source/AreaUpdate/Area.cpp,Source/AreaUpdate/Area.h
# 원본/생성물을 제외한 현재 프로젝트 전체 검사 또는 적용
.\tools\quality\Format-Code.ps1 -All -Check
.\tools\quality\Format-Code.ps1 -All
```

중괄호 자동 제거는 껐다. `clang-tidy`의 `readability-braces-around-statements`와 `ShortStatementLines: 2`로 여러 줄 본문에 중괄호를 요구한다. 이 도구의 줄 수는 조건 끝에서 계산하므로 값 2가 다음 줄의 한 줄짜리 본문을 허용한다. 중괄호 검사는 아래처럼 수행한다. MSVC/Windows SDK를 찾을 수 있는 LLVM 환경이 필요하며 현재 LLVM 22에서 확인했다.

```powershell
.\tools\quality\Check-ControlBraces.ps1       # 검사만
.\tools\quality\Check-ControlBraces.ps1 -Fix  # 여러 줄 본문의 중괄호 추가
.\tools\quality\Format-Code.ps1 -All         # 추가 후 서식 정리
```

명명·지원되는 변수 초기화·중괄호 규칙은 다음 명령으로 함께 검사한다. 이 검사는 소스와 포함된 프로젝트 헤더를 분석하며 원본 스냅샷은 제외한다. 테스트 hook을 켜서 분석하고, 실제 production 구성은 솔루션 빌드로 별도 검증한다.

```powershell
.\tools\quality\Check-CodeStyle.ps1
```

전체 적용을 요청받은 경우에만 `-All`을 사용한다. 논리적 빈 줄, bool/포인터 표기, 수명·잠금 경계는 리뷰로 보완한다.

## VS Code

`.vscode/extensions.json`의 C/C++ 및 EditorConfig 확장을 설치하고 `clang-format`을 PATH에 둔다. C++ 저장 시 `.clang-format`으로 포맷한다. 기존 사용자 설정을 덮어쓰는 확장이 있으면 해당 formatter 설정을 확인한다. EditorConfig는 인코딩·개행 규칙을 보완하고 Git은 `.gitattributes`로 개행을 관리한다.

Git 원본 `Source/AreaUpdateLegacy/Baseline`, 원본 어댑터 `Source/AreaUpdateLegacy/OriginalAreaAdapter.*`, ThirdParty는 일괄 포맷/명명 변경 대상이 아니다. 스냅샷은 저장·편집하지 않는다. 이미 체크아웃된 파일에 Git 개행 설정을 소급 적용하려고 무조건 renormalize하지 않는다.

## 명명 검사와 리뷰

`.clang-tidy`는 명명 검사 설정이다. 실제 분석에는 MSVC/SDK와 맞는 컴파일 옵션 또는 compilation database가 필요하다. 설정을 추가하는 것만으로 저장 시 전체 명명 검사가 실행되지는 않는다. 자동 rename이나 `--fix`는 참조 변경을 검토한 뒤 사용한다.

잠금 순서·소유권·수명·callback 위치·상태 전이·함수 책임은 도구만으로 보장할 수 없다. 리뷰와 테스트로 확인한다. 서식 변경에 공간 반경, batch 크기, 공평성, Job 재전달 정책, 메모리 순서 변경을 섞지 않는다.
