# Animation Injector

**버전 0.12.280** · [English](README_EN.md)

XML 선언과 HKX 파일만으로 Skyrim에 일반 동작과 두 사람이 하는 페어 동작을 추가하는 SKSE 프레임워크입니다. 동작을 추가할 때 Nemesis·Pandora를 다시 실행할 필요가 없습니다. 다른 모드가 요구하는 비헤이비어 패치는 그대로 유지하세요.

이 패키지는 프레임워크 본체입니다. 단축키나 동작을 스스로 추가하지 않으며, Animation Injector용 XML을 담은 동작 모드와 함께 써야 효과가 있습니다.

## 요구 사항

- SKSE64
- Address Library for SKSE Plugins (게임 버전에 맞는 것)
- 인게임 검증 환경: **Skyrim SE/AE 1.6.1170**. CommonLibSSE-NG와 Address Library로 SE 1.5.x~AE 1.6.x를 대상으로 빌드하지만 모든 런타임에서 시험한 것은 아닙니다. VR은 지원하지 않습니다.

## 설치

모드 관리자(MO2 등)로 설치합니다. 포함 파일:

| 파일 | 역할 |
|---|---|
| `SKSE/Plugins/AnimationInjector.dll` | 플러그인 |
| `SKSE/Plugins/AnimationInjector.ini` | 설정 (주석 한/영 병기) |
| `Interface/AnimationInjectorLoading.swf` | 로딩 표시와 QTE를 그리는 빈 화면. 없으면 QTE가 보이지 않습니다 |
| `licenses/` | 종속 라이브러리 라이선스 원문 |

업데이트할 때 INI를 직접 고쳐 썼다면 덮어쓰기 전에 값을 옮겨 두세요.

## 제공 기능

| 구분 | 내용 |
|---|---|
| 시작 | 키보드 키 이름·스캔코드, 게임패드 버튼 이름, 애니메이션 이벤트. 플레이어와 NPC 모두 |
| 일반 동작 | 진입·종료 블렌드, 이동 입력으로 끊는 Recovery, 바닐라 동작으로 끊는 Cancel, 다음 동작으로 잇는 Connect, 시작 제한(지상·공중, 재사용 간격), 클립 시각 기능(무기 숨김·표시, 주문, 상태, 래그돌, 카메라 흔들림, 속도 정지, 시간 배율) |
| 기능 슬롯 | 회전, 무적(IFrame), 경직 저항(HyperArmor), 스태미나·매지카 소모, 공격 데이터 |
| 조건 분기 | `Flow`로 이동 방향·무기·공격 상태·스태미나·공중·누운 방향·장비·종족·성별·확률 등 조건에 따라 재생할 동작 선택 |
| 이동 | HKX의 AMR 주석(`animmotion`, `animrotation`)대로 이동 |
| 페어 | 두 역할이 든 HKX 하나로 페어 재생, 시작 정렬(피해자는 제자리 회전, 가해자 이동), 종료 위치 보정, 상대를 잃었을 때의 종료 블렌드 |
| 이벤트 등록 | HKX 주석 이벤트에 무기 숨김·표시, 무적 켜고 끄기, 소모, 화살 발사(당긴 만큼·바로 발사) 등 기능을 전역으로 연결. 기능은 이벤트를 보낸 액터에게 적용 |
| 주석 명령 | HKX 주석 `AI.CamShake\|세기\|시간`으로 그 순간 카메라 흔들림. 등록 없이 동작하며 숫자는 모두 적어야 함 |
| QTE | 6가지 유형(`splitArc`, `timingBar`, `rotatingNeedle`, `holdRelease`, `rapidPress`, `keySequence`), 결과별 이벤트·소리·다음 동작, 화면에 그리지 않는 입력 전용 QTE(`hidden="true"`) |
| 운반 | 페어로 대상을 들어 올려 운반하고 내려놓기(부분 래그돌) |
| 시점 | 일반 동작은 `view`로 3인칭·1인칭·둘 다 선택(1인칭 HKX는 `_1stPerson` 아래 같은 경로), 1인칭에서 3인칭으로 바꿔 재생하는 옵션 |

페어·운반은 3인칭 전용입니다.

## 선언 예시

XML 위치: `Data/SKSE/Plugins/AnimationInjector/<모드 이름>/<파일>.xml`
HKX 위치: `Data/meshes/actors/character/` 아래 (`clip`은 이 폴더 기준 상대 경로)

```xml
<AnimationSet version="4" id="MyMod">
  <Action id="Slash1" clip="Animations\MyMod\Slash1.hkx">
    <Start key="G" gamepad="RB"/>
    <Flow entry="Weapon">
      <Test id="Weapon" kind="weaponDrawn" subject="self" pass="Play"/>
      <Play id="Play" action="Slash1"/>
    </Flow>
    <Recovery at="event:MyMod_Recovery" blend="0.15"/>
    <IFrame from="0.1" until="0.4"/>
    <Connect to="Slash2" key="G" gamepad="RB"><Window from="0.45"/></Connect>
    <Cancel to="vanilla:Block"/>
  </Action>
  <Action id="Slash2" clip="Animations\MyMod\Slash2.hkx">
    <Exit blend="0.2"/>
  </Action>
</AnimationSet>
```

- 무기를 뽑았을 때만 시작하게 하는 검사는 위처럼 조건 분기(`Flow`)에 둡니다. `<Require>`는 `air`·`cooldown`만 받습니다(0.12.276). 예전 XML의 `weapon`·`blockWhileAttacking`·`stamina`는 `python tools/require_to_flow.py --write <xml>`이 조건 분기로 옮깁니다.
- 시점을 이벤트(`event:이름`)로 두면 그 이벤트가 올 때만 동작합니다. 「안 오면 n초」(`fallback`)는 없어졌습니다(0.12.278).
- 공간·지면 확보(`kind="space"`·`"ground"`)의 `direction`에 본인 기준 `selfForward|selfBack|selfLeft|selfRight`가 생겼습니다. 본인이 그 페어의 시작 자리에 섰을 때 바라보는 방향이 기준입니다(0.12.280). 예전 `partner*`(상대 기준)도 그대로 읽습니다.
- 래그돌은 `<Do op="ragdoll" direction="down|up|forward|backward|left|right" force="0~20"/>`입니다. 페어 중이면 그 액터의 페어 물리 고정을 먼저 풀고 래그돌 뒤 기상을 지켜봅니다(0.12.278, 예전 `releasePhysics`는 거부).
- 루트는 `<AnimationSet version="4">`만 읽습니다. 다른 형식의 파일은 로그에 이유를 남기고 파일 전체를 건너뜁니다.
- 파일 하나는 전부 읽히거나 전부 건너뜁니다. 오류가 하나라도 있으면 그 파일의 동작은 하나도 등록되지 않습니다.
- HKX 경로의 파일·폴더 이름은 ASCII 영문·숫자·일반 기호만 쓸 수 있습니다(한글 불가).
- 키 이름은 물리 키 기준이라 한/영 입력 상태와 무관합니다.
- XML이나 HKX를 바꾼 뒤에는 게임을 완전히 다시 시작하세요.

더 많은 문법은 Animation Injector를 쓰는 공개 동작 모드의 XML을 참고하세요. 참고용 모드 링크는 모드 페이지에 있습니다.

## 설정

`SKSE/Plugins/AnimationInjector.ini`의 주요 항목입니다. 각 항목의 설명은 INI 주석에 있습니다.

| 항목 | 기본값 | 의미 |
|---|---|---|
| `[Compatibility] iNativePairAlignment` | 2 | 페어 시작 정렬. 0 끔 / 1 Animation Injector 페어만 / 2 모든 페어(킬무브 포함). Paired Animation Improvements와 함께 쓰면 1 권장 |
| `[Compatibility] iPairEndPosition` | 2 | 페어 종료 위치 보정. 0 끔 / 1 Animation Injector 페어만 / 2 모든 페어 |
| `[Compatibility] bFireArrow` | true | 동작 중 활 발사 연동 |
| `[Preload] bOrdinary` | true | 일반 동작 애니메이션도 미리 불러 두기 |
| `[Display] bLoadingProgress` | true | 게임 시작·로드 때 준비 진행 표시 |
| `[Debug] bDebug` | false | 상세 로그. 문제를 보고할 때만 켜세요 |

## 호환성과 제한

- OAR·AMR·Precision·MCO/BFCO와 함께 쓰는 구성에서 동작을 확인했습니다. 모든 조합과 모든 종족 그래프를 보장하지는 않습니다.
- Precision이 설치되어 있으면 운반 대상은 들기부터 운반이 끝날 때까지 Precision 타격 판정에서 빠집니다(공개 API `ToggleDisableActor`).
- 무적(IFrame)은 그 액터 하나만 유령 상태로 만듭니다. 같은 베이스를 쓰는 다른 NPC에는 영향이 없습니다.
- 페어는 캐릭터 골격의 `NPC`/`2_` 두 역할이 든 HKX를 씁니다. 단독 HKX 두 개를 자동으로 페어로 합치지 않습니다.
- 상대가 죽거나 래그돌이 되면 남은 쪽은 페어를 끝냅니다(이어서 재생하지 않음).
- 같은 동작 ID(대소문자 무시)가 여러 파일에 있으면 파일 이름 순으로 나중 것이 앞 것을 대체합니다.

## 문제 보고

1. INI에서 `bDebug = true`로 바꾸고 게임을 다시 시작합니다.
2. 문제를 재현합니다.
3. `문서\My Games\Skyrim Special Edition\SKSE\AnimationInjector.log`(직전 실행은 `AnimationInjector.previous.log`)와, 크래시라면 크래시 로거 로그를 함께 보내 주세요. 사용한 동작 모드와 마지막에 실행한 동작도 알려 주세요.

로그 맨 앞에 플러그인 버전과 게임 런타임 버전이 찍힙니다. XML이 읽히지 않으면 `[Decl]`·`[Decl:v2]`로 시작하는 줄에 파일과 거부 이유가 있습니다.

## 라이선스

Animation Injector는 GNU General Public License 3.0(GPL-3.0)으로 배포합니다. 전문은 `LICENSE`, 종속 라이브러리 라이선스는 `licenses/`에 있습니다. CommonLibSSE-NG(alandtse 포크, 고정 커밋)를 사용합니다. 소스는 별도의 소스 패키지로 제공합니다.
