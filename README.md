# Animation Injector

**버전 0.12.285** · [English](README_EN.md)

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
| 일반 동작 | 진입·종료 블렌드, 이동 입력으로 끊는 MoveCancel, 바닐라 동작으로 끊는 Vanilla, 다음 동작으로 잇는 Transition, 시작 제한(지상·공중, 재사용 간격), 클립 시각 기능(무기 숨김·표시, 주문, 상태, 래그돌, 카메라 흔들림, 속도 정지, 시간 배율) |
| 기능 슬롯 | 회전, 무적(IFrame), 경직 저항(HyperArmor), 스태미나·매지카 소모, 공격 데이터 |
| 조건 분기 | 시작·전환 안의 조건 카드(`Check`·`Row`·`True`·`False`)로 이동 방향·무기·공격 상태·스태미나·공중·누운 방향·장비·종족·성별·확률 등 조건에 따라 재생할 동작 선택 |
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
<AnimationSet version="6" id="MyMod">
  <Start key="G" gamepad="RB" cooldown="0.3" to="Slash1">
    <Check kind="weaponDrawn"/>
  </Start>
  <Action id="Slash1" clip="Animations\MyMod\Slash1.hkx">
    <MoveCancel at="event:MyMod_Recovery" blend="0.15"/>
    <IFrame from="0.1" until="0.4"/>
    <Transition to="Slash2" key="G" gamepad="RB" from="0.45"/>
    <Vanilla to="Block"/>
  </Action>
  <Action id="Slash2" clip="Animations\MyMod\Slash2.hkx">
    <Exit blend="0.2"/>
  </Action>
</AnimationSet>
```

- 시작(`<Start>`)은 루트에 씁니다(형식 6, 0.12.291). `to`로 동작을 가리키거나 안에 조건 카드를 둡니다. 카드는 공통 조건(`<Check>`·`<And>`·`<Or>`, 뒤집기는 `not="true"`), 위에서부터 조건이 모두 맞는 첫 행(`<Row to="동작">조건</Row>`), 공통 조건만 쓴 카드의 `<True>`, 아무것도 맞지 않을 때의 `<False>`로 씁니다. 여러 곳이 함께 쓰는 카드는 루트의 `<Condition id="이름">`으로 두고 `condition="이름"`으로 가리킵니다. 전환(`<Transition>`)과 QTE 결과도 같은 방법으로 목적지를 정합니다. 한 동작에 시작을 여러 개 둘 수 있습니다.
- 쿨다운은 시작의 것입니다(`<Start cooldown="0.3">`): 그 시작이 시작시킨 동작이 끝난 뒤 그동안 그 시작만 막습니다. 동작의 공중 규칙은 `air="both"`(땅·공중)·`"only"`(공중만) 속성이고, 생략하면 땅에서만입니다.
- 무기를 뽑았을 때만 시작하게 하는 검사는 위처럼 시작 안의 `<Check>`로 둡니다.
- 시점을 이벤트(`event:이름`)로 두면 그 이벤트가 올 때만 동작합니다. 「안 오면 n초」(`fallback`)는 없어졌습니다(0.12.278).
- 공간·지면 확보(`kind="space"`·`"ground"`)의 `direction`에 본인 기준 `selfForward|selfBack|selfLeft|selfRight`가 생겼습니다. 본인이 그 페어의 시작 자리에 섰을 때 바라보는 방향이 기준입니다(0.12.280). 예전 `partner*`(상대 기준)도 그대로 읽습니다.
- 이벤트 등록의 `<Do>`에는 기능과 그 대상만 씁니다: `op`, 마법의 `plugin`·`form`, 래그돌의 `direction`. 수치(`delay`, `mode`·`damage`, `strength`·`duration`, `stamina`·`magicka`, `factor`, `force`)는 그 이벤트를 보내는 동작이나 페어의 `<EventValues><Event name="이름"><Do op="…" …/></Event></EventValues>`에 씁니다. 이벤트가 오면 보낸 액터가 재생 중인 동작의 수치로 실행하고, 없으면 기본값으로 실행합니다. `cost`·`timeOn`은 동작의 수치가 있을 때만 실행합니다(0.12.281). 같은 이벤트의 같은 op끼리 순서대로 짝짓습니다.
- 타격(0.12.293)은 `<Do op="strike"/>`입니다. 기능이 바꾸는 액터가 공격자이고, 엔진이 그 무기로 때릴 때의 피해(무기·기술·특성·대상 방어구·막기, 난이도는 적용 시)를 계산해 넣습니다. HitFrame 판정 원뿔은 쓰지 않습니다. 등록(또는 시점 기능)에 대상 `pick="combatTarget|partner|crosshair|area"`(기본 전투 대상, 플레이어는 십자선 대상), `hit="hostile|all"`(범위의 맞는 대상), `hand="right|left|both"`, `block="vanilla|ignore"`; 동작의 `<EventValues>`(또는 시점 기능)에 `ratio`(무기 데미지 배율, 기본 1) 또는 `amount`(고정 피해), 범위 `radius`·`angle`(전체 폭, 360 = 원)·`up`·`down`(발 높이 기준)·`edge`(반경 끝 피해 비율), `duration`(초, 그동안 들어온 대상을 한 번씩). 래그돌 중인 대상과 벽 너머 대상은 맞지 않습니다. 피해 뒤(0.12.294) 반응은 등록에 `reaction="none|stagger|push"`, 세기는 값에 경직 `strength="0~1"` 또는 밀쳐내기 `force="0~20"`(감쇠 적용, 죽은 대상 제외)입니다. 진짜 타격처럼 무기 마법부여(플레이어는 충전량 소모)·독(1회분 소모)·「Apply Combat Hit Spell」 특성·임팩트(타격음·피)·피격 이벤트(TESHitEvent)가 따라오며, 등록의 `without="enchantment,poison,perks,impact,hitEvent"`로 뺄 수 있습니다.
- 조건(0.12.296): `<Check kind="previousAction" actions="A,B" within="0.8" ended="any|completed|cancelled"/>`(이 액터가 마지막으로 끝낸 우리 동작, 그 조건을 쓰는 선언이 있을 때만 기록), `kind="falling" option="rising|falling"`(공중, 선택으로 낙하 거리 `compare`·`value`), `kind="slope" option="downhill|uphill|any" compare value`(발밑 경사 도), `kind="actorValue" option="OneHanded|…|level" measure="current|base" compare value`, `kind="gameHour" compare value [upper]`(range는 자정을 넘어도 됨, 20~5), `kind="weather" option="pleasant|cloudy|rainy|snow"`. 모두 조건 카드를 판정할 때만 계산합니다.
- 기능(0.12.296): `<Do op="sound" file="sound\fx\…\a.wav"|plugin form bone="spine|head|rightHand|leftHand|root"/>`(값: `volume` 0~1), `op="globalSet" plugin form restore="end"`(값: `value`; restore면 실행이 끝날 때·저장 뒤 불러올 때 원래 값으로), `op="modEvent" name text`(값: `number`, Papyrus `RegisterForModEvent`가 받음, sender는 그 액터).
- 래그돌은 등록에 `<Do op="ragdoll" direction="down|up|forward|backward|left|right"/>`, 힘은 동작의 `<EventValues>`에 `force="0~20"`입니다. 페어 중이면 그 액터의 페어 물리 고정을 먼저 풀고 래그돌 뒤 기상을 지켜봅니다(0.12.278, 예전 `releasePhysics`는 거부).
- 루트는 `<AnimationSet version="6">`만 읽습니다. 다른 형식의 파일은 로그에 이유를 남기고 파일 전체를 건너뜁니다. 형식 4·5 파일은 `python tools/convert_format6.py --backup <폴더> <xml>...`로 바꿉니다(바꾼 결과가 같은 동작인지는 `AnimationInjectorXmlCheck --compare-starts <옛.xml> <새.xml>`).
- 파일 하나는 전부 읽히거나 전부 건너뜁니다. 오류가 하나라도 있으면 그 파일의 동작은 하나도 등록되지 않습니다.
- HKX 경로의 파일·폴더 이름은 ASCII 영문·숫자·일반 기호만 쓸 수 있습니다(한글 불가).
- 키 이름은 물리 키 기준이라 한/영 입력 상태와 무관합니다.
- 키 대신 게임 동작을 쓸 수 있습니다(0.12.292): `key`·`gamepad` 자리에 `control="jump"`처럼 씁니다. 플레이어가 그 동작에 묶어 둔 키보드·마우스·게임패드 키 모두로 동작하고, 게임 설정에서 키를 바꾸면 그대로 따라갑니다. 시작·전환·QTE·운반 내려놓기와 `<Inputs><Input>`에 쓸 수 있고, 한 요소에는 `input`, `key`·`gamepad`, `control` 중 하나만 씁니다. 그 입력으로 우리 동작이 시작되거나 전환·QTE가 입력을 받으면 게임에는 넘기지 않습니다(점프 키로 시작하면 실제 점프는 하지 않음). 조건이 맞지 않아 아무것도 시작하지 않으면 게임이 평소처럼 받습니다. `onKeyUp="true"`면 누를 때의 게임 동작은 이미 일어난 뒤입니다. 이름: `jump` `sprint` `sneak` `activate` `readyWeapon` `shout` `rightAttack` `leftAttack` `togglePOV` `run` `toggleRun` `autoMove` `wait` `favorites` `tweenMenu`. QTE `keySequence`는 키로만 씁니다.
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
