# Animation Injector

**버전 1.0.3** · [English](README_EN.md)

새 애니메이션을 넣으려면 보통 Nemesis나 Pandora 같은 비헤이비어 도구로 비헤이비어 그래프를 패치해야 합니다. Animation Injector는 그 과정 없이 새 애니메이션을 넣는 SKSE 프레임워크입니다.

어떤 HKX 애니메이션을 언제, 어떤 조건에서 재생할지 XML 파일에 적어 두면, 게임이 시작할 때 그 XML을 읽고 애니메이션을 재생합니다. 애니메이션을 추가하거나 바꿔도 비헤이비어 도구를 다시 실행할 필요가 없습니다.

Animation Injector는 Nemesis·Pandora·OAR로 만든 모드를 대체하거나 바꾸지 않습니다. Animation Injector용으로 만든 모드만 실행하며, 지금 쓰는 비헤이비어 모드는 지금처럼 함께 쓸 수 있습니다.

**아래의 페어 애니메이션 보정을 빼면 이 모드만으로는 아무 일도 일어나지 않습니다.** Animation Injector용으로 만든 애니메이션 모드를 함께 설치하세요.

- **사용자**: Animation Injector용 애니메이션 모드는 설치하면 바로 동작합니다. Nemesis·Pandora를 실행할 필요가 없습니다.
- **모드 제작자**: 비헤이비어 그래프를 패치하지 않고 XML만 써서 새 애니메이션을 게임에 넣을 수 있습니다.

## 동작 방식

Animation Injector는 바닐라 비헤이비어 그래프를 그대로 두고 그 위에서 새 애니메이션을 재생합니다. 예를 들어 특정 애니메이션 이벤트가 일어날 때, 특정 단축키를 누를 때 재생합니다. 바닐라 이벤트를 가로채 바닐라 반응 대신 내 애니메이션을 재생할 수도 있습니다(예: 공격을 막았을 때).

예를 들어 질주를 멈출 때 나오는 정지 애니메이션을 넣으려면, 질주가 끝날 때 발생하는 `sprintStop` 이벤트를 신호로 쓰면 됩니다. Animation Injector Editor로 이 XML을 만드는 과정은 [이 영상](https://www.youtube.com/watch?v=H0bTLvZqs0Q)에서 볼 수 있습니다.

## 요구 사항

- [SKSE64](https://skse.silverlock.org/)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444) (게임 버전에 맞는 것)
- Skyrim SE/AE. 1.6.1170에서 시험했습니다. VR은 지원하지 않습니다.

## 설치

모드 관리자(MO2, Vortex 등)로 설치합니다. 업데이트할 때 INI를 직접 고쳤다면 덮어쓰기 전에 값을 옮겨 두세요.

## 할 수 있는 것

동작 모드가 아래 기능을 조합해 동작을 만듭니다.

| 구분 | 내용 |
|---|---|
| 시작 | 키보드·게임패드 키, 게임 동작(점프·질주 등, 키 설정을 따라감), 애니메이션 이벤트. 플레이어와 NPC 모두. 키는 누르는 순간·짧게·두 번·길게 누름을 구분할 수 있고, 게임 동작의 누름 중 시작이 가져가지 않은 것은 게임에 돌려줌(예: 점프를 짧게 누르면 닷지, 길게 누르면 점프). 동작이 끝나면 이어지는 전환, 키를 떼면 넘어가는 전환, 이동 방향이 바뀌면 넘어가는 전환, 클립 반복 재생, 누른 시간 조건으로 차지 공격(방향별 대시로 움직이며 차지 포함)도 만들 수 있음 |
| 이어 하기 | 키 입력이나 이벤트로 다음 동작으로 연결, 이동 입력이나 다른 행동으로 끊기 |
| 조건 | 무기·이동 방향·공중·스태미나·장비·종족·직전 동작·받은 공격(일반·파워어택·막음 등)·날씨 등에 따라 다른 동작 재생 |
| 동작 중 효과 | 무적, 경직 저항, 회전, 시간 배율, 스태미나·매지카·체력 소모, 타격, 화살 발사, 주문, 래그돌, 소리, 카메라 흔들림 등 |
| HKX 이벤트 | 어느 HKX(다른 모드·바닐라 포함)가 보내는 애니메이션 이벤트에 기능을 등록. HKX 주석 `AI.명령\|값`은 등록 없이 그 시점에 실행 |
| 페어 | 두 사람이 함께 하는 동작. 시작할 때 두 사람의 위치와 방향을 맞추고, 끝날 때 위치를 보정 |
| 운반 | 상대를 들어 올려 옮기고 내려놓기. 물리로 흔들리는 대상의 래그돌 값은 들기 동작마다 XML 옆 파일로 정함(없으면 바닐라) |
| QTE | 타이밍·연타·누르고 떼기·키 순서 등 6가지. 결과에 따라 다른 동작으로 연결 |
| 시점 | 3인칭·1인칭 동작, 1인칭에서 3인칭으로 바꿔 재생 |

페어와 운반은 3인칭에서만 동작합니다.

## 페어 애니메이션 보정

이 모드의 페어뿐 아니라 바닐라 킬무브와 다른 모드의 페어 애니메이션에도 두 가지 보정을 합니다.

- **시작 정렬**: 피해자는 제자리에서 가해자 쪽으로 돌기만 하고, 가해자가 정해진 자리로 이동합니다.
- **종료 위치**: 걸어 나가며 끝나는 애니메이션처럼 루트 본이 배우에게서 멀어지는 경우, 원래는 끝나는 순간 배우가 시작 위치로 되돌아갑니다. 이 모드는 배우를 애니메이션이 끝난 자리에 남깁니다.

이 모드의 페어에만 쓰려면 INI의 `iNativePairAlignment`·`iPairEndPosition`을 1로, 끄려면 0으로 바꾸세요.

## 동작 모드 만들기

동작은 XML 파일로 정의하며, 직접 쓰거나 모드 제작자용 도구인 Animation Injector Editor로 만들 수 있습니다. 에디터에서는 HKX를 카드로 놓고 연결하면 게임이 읽는 XML과 모드 폴더를 만들어 줍니다. 플레이어에게는 에디터가 필요 없고, Animation Injector와 이를 쓰는 애니메이션 모드만 있으면 됩니다.

- HKX 경로의 파일·폴더 이름에는 영문·숫자·일반 기호만 쓰세요. 한글 경로의 파일은 게임이 찾지 못합니다.
- XML이나 HKX를 바꾼 뒤에는 게임을 완전히 다시 시작하세요.
- XML 파일에 오류가 하나라도 있으면 그 파일의 동작은 하나도 들어가지 않습니다. 이유는 로그에 남습니다.

XML을 직접 쓰려면 `XML_GUIDE.md`를 보세요.

## 설정

`SKSE/Plugins/AnimationInjector.ini`의 주요 항목입니다. 나머지 항목은 INI의 설명을 보세요.

| 항목 | 기본값 | 의미 |
|---|---|---|
| `iNativePairAlignment` | 2 | 페어 시작 때 위치·방향 맞추기. 0 끔 / 1 이 모드의 페어만 / 2 모든 페어(킬무브 포함). |
| `iPairEndPosition` | 2 | 페어가 끝날 때 위치 보정. 0 끔 / 1 이 모드의 페어만 / 2 모든 페어 |
| `bOrdinary` | true | 혼자 하는 동작의 애니메이션도 미리 불러 둠(페어는 항상 미리 불러 둠) |
| `bLoadingProgress` | true | 게임 시작·로드 때 준비 진행 표시 |
| `bDebug` | false | 상세 로그. 문제를 보고할 때만 켜세요 |

## 호환성

- OAR·AMR·Precision·MCO/BFCO와 함께 쓰는 구성에서 확인했습니다. 모든 모드 조합을 보장하지는 않습니다.
- True Directional Movement가 있으면 이동 방향 조건과 동작의 회전 구간이 TDM 설정을 따릅니다. 끄려면 INI의 `bFollowTDM = false`.
- 페어 도중 상대가 죽거나 쓰러지면 페어가 끝납니다.
- 동작 이름은 XML 파일(AnimationSet ID)마다 따로라서, 동작 모드끼리 같은 이름을 써도 서로 덮어쓰지 않습니다. 두 XML이 같은 AnimationSet ID를 쓰면 뒤 파일을 따로 불러오고 로그에 경고합니다.

## 문제 보고

1. INI에서 `bDebug = true`로 바꾸고 게임을 다시 시작합니다.
2. 문제를 재현합니다.
3. `문서\My Games\Skyrim Special Edition\SKSE\AnimationInjector.log`(직전 실행은 `AnimationInjector.previous.log`)를 보내 주세요. 크래시라면 크래시 로거의 로그도 함께 보내 주세요. 사용한 동작 모드와 마지막에 한 동작도 알려 주세요.

게임이 메인 메뉴 전에 크래시 로그 없이 꺼지면, 한 번 더 실행한 뒤 `AnimationInjector.log`를 보내 주세요. 두 번째 실행이 첫 실행이 어느 단계에서 멈췄는지, Windows가 기록한 오류(멈춘 모듈·예외 코드)를 `[Startup]` 줄로 남깁니다.

## 크레딧

- **SKSE Team**: [Skyrim Script Extender](https://skse.silverlock.org/)
- **meh321**: [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
- **Ryan McKenzie**: [CommonLibSSE](https://github.com/Ryan-rsm-McKenzie/CommonLibSSE)
- **CharmedBaryon**: [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG)
- **alandtse**와 기여자들: [CommonLibSSE-NG](https://github.com/alandtse/CommonLibVR/tree/ng) (이 모드를 빌드한 버전)
- **SARDONYX**: [serde-hkx](https://github.com/SARDONYX-sard/serde-hkx) (에디터의 HKX 읽기)
- **Lukas Cone (PredatorCZ)**: [HavokLib](https://github.com/PredatorCZ/HavokLib) (에디터의 스플라인 애니메이션 압축 해제, 포팅)
- 라이브러리: pugixml, spdlog, fmt, rapidcsv, DirectXTK, DirectXMath, xbyak, hde64 (런타임) · Tauri, React, React Flow, three.js (에디터)

## 라이선스

GNU General Public License 3.0(`LICENSE`). 소스 코드의 위치와 이 배포물을 빌드한 커밋은 `SOURCE.txt`에 있습니다. 함께 쓰는 라이브러리의 라이선스는 `licenses/`에 있습니다.
