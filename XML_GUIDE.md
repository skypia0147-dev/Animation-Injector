# Animation Injector XML 작성 안내

[English](XML_GUIDE_EN.md)

동작 모드는 Animation Injector Editor로 만드는 것을 권합니다. 이 문서는 XML을 직접 쓰고 싶은 분을 위한 길잡이입니다. 자주 쓰는 것만 다룹니다. 여기 없는 기능은 에디터로 만든 뒤 **XML 미리보기(Ctrl+`)** 로 어떻게 쓰이는지 보면 됩니다. 직접 쓴 XML도 에디터에서 열면 카드가 자동으로 배치되므로, 언제든 에디터로 옮겨 갈 수 있습니다.

이 문서의 예시는 모두 Animation Injector가 그대로 읽는 파일입니다.

## 파일 위치

| 무엇 | 어디에 |
|---|---|
| XML | `Data\SKSE\Plugins\AnimationInjector\<모드 이름>\<아무 이름>.xml` |
| HKX | `Data\meshes\actors\character\` 아래. `clip`에는 이 폴더 기준 경로를 씁니다 |
| 운반 래그돌 값 | XML 옆 `Ragdoll\<들기 동작 id>.xml` ([운반 래그돌](#운반-래그돌)) |

- XML은 모드 이름 폴더 바로 안에 둡니다(더 아래 폴더는 읽지 않음).
- HKX 경로의 파일·폴더 이름에는 영문·숫자·일반 기호만 씁니다.
- XML이나 HKX를 바꾼 뒤에는 게임을 완전히 다시 시작해야 반영됩니다.
- 파일 하나에 오류가 하나라도 있으면 그 파일 전체가 무시됩니다. 게임에 넣기 전에 [검사](#검사하기)하세요.

## 가장 작은 파일

G 키(패드 RB)를 누르면 구르기 HKX를 재생합니다.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="MyMod">
  <Start key="G" gamepad="RB" cooldown="0.5" to="Roll"/>
  <Action id="Roll" clip="Animations\MyMod\Roll.hkx">
    <Exit blend="0.2"/>
  </Action>
</AnimationSet>
```

- `<?xml … ?>`: XML 표준 선언(이 파일은 UTF-8로 저장된 XML이라는 표시). 빼도 되지만, 쓴다면 파일 맨 첫 줄에 두고 UTF-8로 저장합니다.
- `<AnimationSet>`: 파일의 맨 바깥. `version="6"`은 그대로 쓰고, `id`는 모드마다 다른 이름(영문·숫자·`_`)으로 정합니다.
- `<Start>`: 동작을 시작하는 방법. `to`가 재생할 동작입니다. `cooldown`(초)이 지나기 전에는 다시 시작하지 않습니다.
- `<Action>`: HKX 하나를 재생하는 동작. `id`는 파일 안에서 겹치지 않게 정합니다. 다른 모드에 같은 `id`가 있어도 서로 섞이지 않습니다.
- `<Exit>`: 끝날 때 원래 동작으로 돌아가는 블렌드 시간(초).

## 동작 이어 하기

무기를 뽑은 상태에서 G를 누르면 베기 1을 하고, 그 도중에 G를 다시 누르면 베기 2로 이어집니다.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="MyMod">
  <Start key="G" gamepad="RB" cooldown="0.3" to="Slash1">
    <Check kind="actorStatus" option="weaponDrawn"/>
  </Start>
  <Action id="Slash1" clip="Animations\MyMod\Slash1.hkx" enter="0.1">
    <IFrame from="0.1" until="0.4"/>
    <HyperArmor until="0.6"/>
    <Cost stamina="20"/>
    <Transition to="Slash2" key="G" gamepad="RB" from="0.45" until="end-0.1"/>
    <MoveCancel at="0.8" blend="0.25"/>
    <Exit blend="0.2"/>
  </Action>
  <Action id="Slash2" clip="Animations\MyMod\Slash2.hkx">
    <MoveCancel at="event:MyMod_Recovery" blend="0.2"/>
    <Exit at="end-0.1" blend="0.2"/>
  </Action>
</AnimationSet>
```

| 요소 | 뜻 |
|---|---|
| `<Transition to key from until>` | `from`~`until` 사이에 키를 누르면 `to` 동작으로 넘어갑니다. 키 대신 `event="이름"`도 됩니다. 키 전환은 `press="tap\|double\|hold"`로 누르는 방식을 가를 수 있습니다(아래 「누름 종류」, 1.0.8 이상) |
| `<Transition to on="end" blend>` | 이 동작이 끝나는 순간(종료 지점) 끝내는 대신 `to`로 넘어갑니다. 동작마다 하나, 키·`from`·`until` 없음. 시각으로 끝나는 동작에서만(`<Exit at="event:…">`과 함께 쓸 수 없음) |
| `<Transition to on="release" from until>` | 차지 종료: 구간 안에서 키가 떼져 있으면 넘어갑니다. "뗀 순간"이 아니라 상태라 구간 전에 뗐으면 구간이 열리는 순간 넘어갑니다. 키는 `key`·`gamepad`·`control`로 정하고, 쓰지 않으면 이 동작을 시작한 키(시작 키나 키 전환의 키, 끝나면·키를 떼면으로 이어지는 동안 유지). NPC는 반복 동작의 `maxHold`에서만 뗀 것으로 봅니다 |
| `<Transition to on="move" from until buffer>` | 이동 전환: **차지 중**(이 동작을 시작한 키를 누르고 있는 동안)에 이동키를 누르면 넘어갑니다. 차지 키를 떼면 이동키는 아무것도 하지 않고 선입력도 버립니다(그때는 `on="release"`). 닷지처럼 키를 짧게 누르는 동작은 이동키+닷지 키를 쓰는 키 전환(`<Transition condition="방향 카드" key=…>`)으로 잇습니다. 키 전환과 같아서 구간 안에서 누르면 바로, 구간이 열리기 전에 누른 것도 선입력으로 받아 열리는 순간 넘어갑니다(`buffer="false"`면 선입력 없음). 구간이 열릴 때 이동키를 누르고 있는 것도 선입력으로 칩니다 — 누르고 있는 동안 한 번씩 이어 재생되고, 떼면 재생 중인 동작은 끝까지 재생하고 끝납니다. 조건 카드는 누르고 있는 방향(이미 뗐으면 누른 방향)으로 고릅니다(이동 방향 조건과 같은 판정, TDM 반영). 키·이벤트 없음, 플레이어만. 같은 동작으로 가면 그 동작을 처음부터 다시 재생하고, 반복 동작이 같은 동작을 고르면 끊지 않고 이어 재생합니다(차지는 그대로 유지). 방향별 대시를 조건 카드(`movementDirection`)로 고르는 데 씁니다 |
| `<Transition end="true" …>` | 끝내기 전환(1.0.8 이상): 다른 동작으로 가지 않고 이 동작을 바로 끝냅니다(`<Exit blend>`로 빠져나옴). 조건은 다른 전환과 같습니다: 키(`key`·`gamepad`·`control`·`input`, `press`), `event`, `on="release"`, `on="move"`, 구간(`from`·`until`·`buffer`). `to`·조건 카드·`blend`·`partnerTo`·`on="end"`는 쓰지 않습니다. 일반 동작만. 예: 이벤트로 시작한 반복 동작을 종료 이벤트로 끝내기 `<Action id="Loop" clip="…" loop="true" maxHold="600"><Transition end="true" event="StopLoop"/></Action>` |
| `<MoveCancel at blend>` | `at` 이후 이동 키를 누르면 동작을 끊고 걷기·달리기로 돌아갑니다. 전투 중인 NPC는 `at`부터 AI에게 이동을 돌려주고, AI가 움직이거나 공격하려 하면 끊습니다(가만히 있으면 끝까지 재생) |
| `<IFrame from until>` | 그 사이 무적 |
| `<HyperArmor from until>` | 그 사이 경직 저항 |
| `<TimeScale factor from until ease>` | 그 사이 세상 전체의 시간 배율(플레이어가 재생할 때만). `factor` 0.05~5(0.5면 절반 속도), `ease`는 배율까지·구간 끝에서 원래 속도까지 바뀌는 데 걸리는 실제 초(0~2, 쓰지 않으면 한 번에). 동작이 중간에 끊겨도 원래 속도로 돌아옵니다 |
| `<Rotate from until>` | 그 사이 방향을 돌릴 수 있음. True Directional Movement는 동작 중 이동 입력을 받지 않으므로, 그 사이 이동 키를 누르고 있으면 런타임이 TDM 설정대로 돌립니다(바닐라 스타일: 카메라 정면, 방향성: 카메라 기준 누른 키 방향) |
| `<Cost stamina magicka health measure at>` | 자원 소모: 스태미나·매지카·체력(체력은 마지막 1은 남김). `measure="percent"`면 각 최대치의 %(0~100), 쓰지 않으면 수치 그대로 |
| `<Exit at blend>` | 언제 끝내고 몇 초 동안 섞을지. `at`을 쓰지 않으면 클립 끝 0.05초 전 |

`<Action>`의 속성: `enter`(들어갈 때 블렌드 초), `air="both"`(땅·공중 모두) / `"only"`(공중에서만, 쓰지 않으면 땅에서만), `view="first"`·`"both"`(1인칭 동작, 1인칭 HKX는 `_1stPerson` 아래 같은 경로), `otherView="switch"`(시점 전환, 1.0.8 이상: 3인칭 동작을 1인칭에서, 1인칭 동작을 3인칭에서 시작하면 동작의 인칭으로 카메라를 바꿔 재생하고 끝나면 원래 인칭으로 돌아옵니다. 쓰지 않으면 다른 인칭에서는 시작하지 않습니다. `view="both"`와 1인칭 페어에는 쓸 수 없습니다. 1.0.7까지의 `firstPerson`은 읽지 않습니다), `loop="true" maxHold="3"`(반복 재생: 클립을 끝없이 이어 재생하고, `maxHold`초(기본 3)가 지나면 키를 뗀 것으로 봅니다(차지를 시작한 키를 아직 누르고 있으면 그 키를 누른 순간부터 세어, 방향별 반복 동작을 오가도 시계는 하나). 키를 떼면 전환이 없으면 그때 끝냅니다. 일반 동작만, `<Exit at>`·끝나면 전환은 쓸 수 없습니다. 무적·회전 같은 기능 시각은 동작 시작부터 세고 `end-…`를 쓸 수 없습니다. 전환의 감지 구간(`from`·`until`)은 **바퀴마다** 다시 열리고 닫히며 `until="end"`·`end-…`는 그 바퀴의 끝입니다. HKX 주석 이벤트는 바퀴마다 다시 옵니다).

**차지 예시**: `<Start control="rightAttack" press="hold" to="ChargeStart"/>` → ChargeStart의 `<Transition to="ChargeLoop" on="end"/>`와 `<Transition condition="ChargeLevel" on="release" from="0.2"/>` → `<Action id="ChargeLoop" loop="true" maxHold="3">`의 `<Transition condition="ChargeLevel" on="release"/>` → 조건 카드 ChargeLevel에서 `<Check kind="heldTime" compare="ge" value="1.5"/>`로 강·약 발동.

**시각 쓰는 법**: `0.4`(클립 시작부터 초), `end`(클립 끝), `end-0.1`(끝 0.1초 전), `event:이름`(HKX 주석 이벤트가 오는 순간).

## 조건으로 고르기

질주 키를 누르면 이동 방향에 따라 다른 회피를 합니다. 무기를 뽑았고 스태미나가 10 이상일 때만 합니다.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="MyDodge">
  <Start control="sprint" cooldown="0.3">
    <Check kind="actorStatus" option="weaponDrawn"/>
    <Check kind="resource" option="stamina" measure="current" compare="ge" value="10"/>
    <Row to="DodgeBack"><Check kind="movementDirection" option="back"/></Row>
    <Row to="DodgeLeft"><Check kind="movementDirection" option="left"/></Row>
    <Row to="DodgeRight"><Check kind="movementDirection" option="right"/></Row>
    <False/>
  </Start>
  <Action id="DodgeBack" clip="Animations\MyDodge\Back.hkx">
    <IFrame until="0.4"/>
    <MoveCancel at="0.6" blend="0.3"/>
  </Action>
  <Action id="DodgeLeft" clip="Animations\MyDodge\Left.hkx">
    <IFrame until="0.4"/>
    <MoveCancel at="0.6" blend="0.3"/>
  </Action>
  <Action id="DodgeRight" clip="Animations\MyDodge\Right.hkx">
    <IFrame until="0.4"/>
    <MoveCancel at="0.6" blend="0.3"/>
  </Action>
</AnimationSet>
```

`<Start>` 안에 조건을 쓰는 순서:

1. 맨 위의 `<Check>`는 공통 조건입니다. 모두 맞아야 아래로 갑니다.
2. `<Row to="동작">`은 위에서부터 보고, 안의 조건이 모두 맞는 첫 행의 동작을 재생합니다. `match="any"`를 쓰면 하나만 맞아도 됩니다.
3. `<False>`는 아무것도 맞지 않을 때입니다. 비워 두면 아무것도 하지 않습니다. `<False to="동작"/>`으로 다른 동작을 재생할 수도 있습니다.
4. 행 없이 공통 조건만 쓸 때는 `<True to="동작"/>`을 씁니다.

- 조건을 뒤집으려면 `not="true"`를 붙입니다. 여럿 중 하나만 맞으면 되는 조건은 `<Or>…</Or>`로 묶습니다.
- 여러 곳에서 같은 조건을 쓰려면 루트에 `<Condition id="이름">`으로 한 번 쓰고 `condition="이름"`으로 가리킵니다.
- `<Transition>`과 QTE 결과도 같은 방법으로 조건을 쓸 수 있습니다.

자주 쓰는 조건:

| `kind` | 더 쓰는 속성 | 참일 때 |
|---|---|---|
| `actorStatus` | `option="weaponDrawn\|combat\|sneaking\|blocking\|sprinting\|swimming\|mounted\|staggered\|ragdoll\|recovering\|bleedingOut\|killmove\|paired\|attacking"`(쉼표로 여럿이면 그중 하나) | 그 상태일 때. `attacking`은 공격 단계(휘두르기·활 당기기·방패 밀치기 등) 중이거나 그래프의 `IsAttacking`이 켜졌을 때(BFCO처럼 비헤이비어 모드가 재생하는 공격) |
| `air` | `option="any\|falling\|rising"`; `falling`·`rising`은 `compare` `value`(떨어진 거리)도 | 공중에 있을 때(아무거나 / 떨어지는 중 / 오르는 중) |
| `protection` | `option="essential\|protected"` | 필수·보호 대상일 때 |
| `female` `interior` `child` | 없음 | 그럴 때 |
| `resource` | `option="health\|stamina\|magicka"` `measure="current\|percent"` `compare` `value` | 값 비교 |
| `equippedType` | `hand="left\|right"` `type` | 그 손에 그 종류: `sword` `dagger` `axe` `mace` `greatsword` `battleaxe` `warhammer` `bow` `crossbow` `staff` `shield` `spell` `torch` `unarmed` |
| `attackState` | `state` | 공격 단계: `none` `draw` `swing` `hit` `bowDraw` `bowDrawn` `bowReleased` 등 |
| `movementDirection` | `option="forward\|back\|left\|right\|none"` | 이동 키 방향. True Directional Movement의 방향성 모드에서는 캐릭터가 입력 쪽으로 돌아 앞으로 달리므로, 이동 키를 누르고 있으면 항상 `forward`입니다(TDM이 없거나 다른 모드면 키 그대로) |
| `chance` | `value`(0~100) | 그 확률로 |
| `magicEffect` `perk` `spell` `race` `faction` `keyword` | `plugin="모드.esp"` `form="16진수 ID"` | 그것을 가졌을 때 |
| `keyword` `wornKeyword` `equippedKeyword` | `keyword="WeapTypeClaw"`(Editor ID) — `plugin` `form` 대신. `equippedKeyword`는 `option="left\|right\|either"` | 그 키워드를 가졌을 때(본인·착용한 장비·그 손의 장비). 이름은 대소문자를 가리지 않고, 두 플러그인이 같은 이름을 쓰면 나중에 로드된 쪽. 없는 이름이면 로그에 경고하고 검사 불능 |
| `global` | `plugin` `form` `equals` | 글로벌 값이 같을 때 |
| `previousAction` | `actions="A,B"`(이 파일의 동작) `within="초"` | 직전에 그 동작을 했을 때 |
| `weather` | `option="pleasant\|cloudy\|rainy\|snow"` | 그 날씨 |
| `hitReceived` | `option="normal\|power\|bash\|sneak\|blocked"` `within="초"` | 마지막으로 맞은 무기 공격이 일반(파워어택·방패 밀치기 아님)·파워어택·방패 밀치기·기습·막음일 때. 마법은 세지 않음. 막았을 때 `<Start event="blockHitStart">`에 `within="0.5"`로 씁니다 |
| `recentEvent` | `events="이벤트,이벤트"` `within="초"` | 그 애니메이션 이벤트 중 하나를 그 시간 안에 받았을 때(그래프로 보낸 이벤트와 그래프가 보낸 이벤트 모두). 질주 공격 직후의 `sprintStop`을 거르려면 `<Check kind="recentEvent" events="attackStop" within="0.5" not="true"/>` |
| `foot` | `option="left\|right"` | 마지막으로 땅에 닿은 발이 왼발·오른발일 때(이동 애니메이션의 `FootLeft`·`FootRight` 중 나중에 온 것, 속도와 상관없음). 멈출 때 발에 맞는 정지 동작: `<Start event="moveStop" actor="player"><Row to="StopLeft"><Check kind="foot" option="left"/></Row><False to="StopRight"/></Start>` |
| `gameHour` | `compare` `value` | 게임 시각 비교 |
| `heldTime` | `compare` `value` | 이 동작을 시작한 키를 누르고 있던 시간(초). 차지 종료 전환이 고를 때는 누른 순간부터 뗀 순간까지. 플레이어만(NPC는 판정 불가) |
| `space` | `distance` `direction="selfForward\|selfBack\|selfLeft\|selfRight"` | 그 방향에 그만큼 빈 공간이 있을 때 |

`compare`는 `lt`(작다) `le`(작거나 같다) `eq`(같다) `ge`(크거나 같다) `gt`(크다) `range`(`value`~`upper` 사이)입니다.

## 이벤트로 기능 실행하기

HKX에 넣은 주석 이벤트가 오면 기능을 실행합니다. 이벤트와 기능은 `<EventRegistry>`에 한 번 등록하고, 세기 같은 값은 동작마다 `<EventValues>`에 씁니다. 클립의 정해진 시각에 실행하려면 `<Do at="초">`를 씁니다.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="MyMod">
  <EventRegistry>
    <Event name="MyMod_Sheathe">
      <Do op="hide"/>
    </Event>
    <Event name="MyMod_Draw">
      <Do op="show"/>
    </Event>
    <Event name="MyMod_Kick">
      <Do op="strike" pick="combatTarget" reaction="stagger"/>
    </Event>
  </EventRegistry>
  <Start key="V" to="Kick"/>
  <Action id="Kick" clip="Animations\MyMod\Kick.hkx">
    <EventValues>
      <Event name="MyMod_Kick">
        <Do op="strike" ratio="1.5" strength="0.5"/>
      </Event>
    </EventValues>
    <Do at="0.35" op="shake" strength="0.4" duration="0.3"/>
    <Do at="0.35" op="sound" file="sound\fx\mymod\kick.wav"/>
    <Exit blend="0.2"/>
  </Action>
</AnimationSet>
```

기능은 이벤트를 보낸 액터(그 동작을 재생하는 액터)에게 적용됩니다.

| `op` | 하는 일 | 주요 값 |
|---|---|---|
| `hide` / `show` | 무기·방패 숨기기 / 다시 보이기 | |
| `magic` | 마법 효과: `action="cast"` 자신에게 한 번 시전, `action="add"` 붙여 두기(능력·질병이 아닌 주문은 시전), `action="remove"` 떼기(원래 가진 것도) | `action` `plugin` `form` |
| `iframeOn` / `iframeOff` | 무적 켜기 / 끄기 | |
| `armorOn` / `armorOff` | 경직 저항 켜기 / 끄기 | |
| `rotateOn` / `rotateOff` | 방향 돌리기 허용 / 끄기 | |
| `cost` | 자원 소모(체력은 마지막 1은 남김) | `stamina` `magicka` `health`; 등록의 `measure="percent"`면 최대치의 % |
| `shake` | 카메라 흔들기(플레이어) | `strength` `duration` |
| `timeOn` / `timeOff` | 시간 배율 바꾸기 / 되돌리기(플레이어). 일반 동작의 시각 기능으로는 쓰지 않고 `<TimeScale>`을 씁니다 | `factor`(0.05~5, 0.5면 절반 속도), `ease`(배율까지·되돌릴 때 걸리는 실제 초, 0~2) |
| `stopVelocity` | 미끄러지는 속도 멈추기 | |
| `strike` | 무기 피해로 때리기 | 대상 `pick="combatTarget\|partner\|crosshair\|area"`, 반응 `reaction="none\|stagger\|push"`; 값 `ratio`(무기 피해 배율) 또는 `amount`(고정 피해), `strength`(경직)·`force`(밀쳐내기) |
| `ragdoll` | 래그돌로 넘어뜨리기 | `direction="down\|up\|forward\|backward\|left\|right"`; 값 `force`(0~20) |
| `fireArrow` | 장착한 화살을 조준점으로 쏘기(플레이어) | 값 `damage`(풀차징 피해의 %) |
| `sound` | 소리 재생 | `file="sound\…\a.wav"` 또는 `plugin` `form`; 값 `volume`(0~1) |
| `globalSet` | 글로벌 값 바꾸기 | `plugin` `form` `restore="end"`(끝나면 되돌림); 값 `value` |
| `modEvent` | Papyrus에 모드 이벤트 보내기 | `name` `text`; 값 `number` |
| `cancel` | 페어 끝내기 | |
| `gripHold` | 무기 잡기 유지: 그 순간 손의 무기 뼈(오른손 Weapon, 왼손 Shield) 값을 동작이 끝난 뒤에도 유지 ([무기 잡기 유지](#무기-잡기-유지)) | `hand="right\|left\|both"`, `view="third\|first\|both"`(기본 third) |
| `gripRelease` | 잡기 유지 풀기(바로) | `hand`, `view` |

켜 두는 기능(숨기기·무적 등)은 짝 기능(`show`·`iframeOff` 등)이 오거나 이어지는 동작이 모두 끝나면 꺼집니다. 다만 등록된 이벤트로 켠 `iframeOn`·`armorOn`·`rotateOn`·`timeOn`은 짝 기능을 등록한 이벤트가 있으면 애니메이션이 끝나도 그 이벤트가 올 때까지 유지됩니다(사망·래그돌·불러오기 때는 꺼짐). 단, 켜는 이벤트를 보낸 HKX가 그보다 뒤에 끄는 이벤트도 보내면 구간처럼 동작합니다: 끄는 이벤트가 오거나 그 HKX가 끝나거나 끊기면 꺼집니다. 우리 동작이든 다른 모드·바닐라 HKX든 같습니다. 등록의 켜는 기능에 `duration`(초, 0~60)을 주면 그 시간이 지나면 꺼집니다. 그 전에 끄는 이벤트가 오면 그때 꺼지고, HKX가 끝나거나 끊겨도 남은 시간은 유지됩니다(시간 배율은 실제 초, 그 밖은 게임 시간).

이벤트로 실행하는 기능에는 `delay`(초)를 값으로 주어 늦출 수 있습니다(활 발사 제외). 페어의 `<Do at>`에는 `target="initiator\|receiver\|both\|player"`로 누구에게 적용할지 정합니다.

## HKX 주석 명령 (`AI.`)

HKX 주석에 `AI.명령|값|값`을 넣으면 등록 없이 그 시각에 기능이 실행됩니다. 값은 주석에 적은 그대로 쓰므로 다른 모드·바닐라 HKX에서도 똑같이 동작합니다. 명령 이름은 대소문자를 가리지 않고, 값은 모두 적어야 합니다. 기능은 주석을 보낸 액터에게 적용됩니다.

| 명령 | 하는 일 |
|---|---|
| `AI.IFrameOn[\|지속]` / `AI.IFrameOff` | 무적 켜기 / 끄기 |
| `AI.ArmorOn[\|지속]` / `AI.ArmorOff` | 경직 저항 켜기 / 끄기 |
| `AI.RotateOn[\|지속]` / `AI.RotateOff` | 방향 돌리기 허용 / 끄기 |
| `AI.TimeOn\|배율\|전환[\|지속]` / `AI.TimeOff` | 시간 배율(플레이어). 배율 0.05~5, 전환 0~2 실제 초 |
| `AI.Hide` / `AI.Show` | 무기 숨기기 / 보이기 |
| `AI.MagicAdd\|플러그인\|폼` · `AI.MagicRemove\|…` · `AI.MagicCast\|…` | 마법 효과 붙이기 · 떼기 · 시전 |
| `AI.Cost\|스태미나\|매지카\|체력` · `AI.CostPercent\|…` | 자원 소모(양 / 최대치의 %) |
| `AI.CamShake\|세기\|시간` | 카메라 흔들기(플레이어) |
| `AI.StopVelocity` | 미끄러지는 속도 멈추기 |
| `AI.Ragdoll\|방향\|힘` | 래그돌(방향 down·up·forward·backward·left·right, 힘 0~20) |
| `AI.FireArrow\|피해` | 화살 쏘기(풀차징 피해의 %, 플레이어) |
| `AI.Sound\|파일\|뼈\|음량` · `AI.SoundForm\|플러그인\|폼\|뼈\|음량` | 소리 재생(뼈 spine·head·rightHand·leftHand·root) |
| `AI.GlobalSet\|플러그인\|폼\|값` | 글로벌 값 바꾸기(되돌리기 없음) |
| `AI.ModEvent\|이름\|글\|숫자` | Papyrus에 모드 이벤트 보내기 |
| `AI.CancelPair` | 페어 끝내기 |
| `AI.GripHold\|손\|시점` | 무기 잡기 유지 (손 right·left·both, 시점 third·first·both) |
| `AI.GripRelease\|손\|시점` | 잡기 유지 풀기 |

타격(`strike`)은 설정이 많아 명령이 없습니다. 이벤트 등록으로 씁니다. 켜는 명령의 끄기는 위의 규칙을 따르고, 끄는 명령은 언제든 보낼 수 있으므로 끄는 명령을 넣지 않으면 사망·래그돌·불러오기 전까지 켜져 있습니다. 명령과 등록 이벤트는 같은 기능으로 취급합니다(`AI.IFrameOff`는 등록 이벤트가 켠 무적도 끕니다).

## 무기 잡기 유지

무기를 역수로 고쳐 잡는 동작처럼, 동작이 끝난 뒤에도 무기를 쥔 모양을 그대로 두고 싶을 때 씁니다(1.0.9 이상). H를 누르면 역수로 고쳐 잡고 그 그립을 유지하며, J를 누르면 다시 정상으로 잡습니다.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="MyGrip">
  <Start key="H" to="Reverse">
    <Check kind="actorStatus" option="weaponDrawn"/>
  </Start>
  <Start key="J" to="Normal">
    <Check kind="actorStatus" option="weaponDrawn"/>
  </Start>
  <Action id="Reverse" clip="Animations\MyGrip\ReverseGrip.hkx">
    <Do at="0.6" op="gripHold" hand="right"/>
    <Exit blend="0.2"/>
  </Action>
  <Action id="Normal" clip="Animations\MyGrip\NormalGrip.hkx">
    <Do at="0" op="gripRelease" hand="right"/>
    <Exit blend="0.2"/>
  </Action>
</AnimationSet>
```

- `gripHold`는 그 순간 손에 대한 무기 뼈의 위치·회전(오른손 `Weapon`, 왼손 `Shield`)을 기억하고, 그 뒤 재생되는 모든 애니메이션(공격, 부위 동작 포함)에서 그 값으로 유지합니다. 몸과 팔은 애니메이션대로 움직이고 무기를 쥔 모양만 남습니다.
- `gripRelease`는 그 프레임에 바로 풀고 애니메이션 값으로 돌아갑니다. 풀기 동작의 첫 프레임을 역수 자세로 만들면 이음매가 보이지 않습니다.
- 저절로 풀리는 때: 무기를 넣을 때, 그 손의 장비가 바뀔 때, 죽음, 래그돌, 세이브 불러오기.
- `view`: 유지할 시점(`third` 3인칭, `first` 1인칭, `both` 둘 다). 동작이 재생된 시점에서 잡은 값을 다른 시점에 옮겨 쓰므로, 그 동작에 1인칭 HKX가 없어도 1인칭에서 유지됩니다.
- 왼손은 바닐라 보정(`DW_ShieldAdjustment`)이 Shield 뼈를 덮어써서, Left Hand Equipment Overhaul 2.0 이상이 있어야 보입니다. 왼손 역수 HKX는 기본 자세 기준으로 만들고 0초에 `Adjustment_Additive` 주석을, 풀기 동작 끝에 `Adjustment_Off` 주석을 넣습니다. 다른 시점으로 옮긴 왼손 유지에는 Animation Injector가 그 시점에 `Adjustment_Additive`를 보내고, 유지가 끝나면 `Adjustment_Off`를 보냅니다.
- 등록 이벤트로도 씁니다: `<Event name="…"><Do op="gripHold" hand="right" view="both"/></Event>`. 그 이벤트 이름을 주석으로 보내는 HKX라면 우리 동작이든 다른 모드·바닐라·OAR 애니메이션이든 실행됩니다. HKX 주석 명령은 `AI.GripHold|right|third`, `AI.GripRelease|right|third`입니다.

## 이동하면서 상체·팔만 (body)

`<Action>`에 `body`를 쓰면 그 동작은 상체나 한쪽 팔에만 재생되고, 다리는 이동 애니메이션 그대로라 **재생하는 동안 자유롭게 걷고 달릴 수 있습니다**(물약 마시기, 팔짱, 손 흔들기 같은 연출). 3인칭에서만 재생합니다.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="WalkingIdles">
  <Start key="Num1" to="CrossArms"/>
  <Start key="Num2" to="Wave"/>
  <Action id="CrossArms" clip="Animations\MyMod\CrossArms.hkx" body="upper" loop="true">
    <Transition key="Num1" end="true"/>
  </Action>
  <Action id="Wave" clip="Animations\MyMod\Wave.hkx" body="rightArm" enter="0.2">
    <Exit blend="0.4"/>
  </Action>
</AnimationSet>
```

- `body`: `upper`(척추부터 위 전부), `rightArm`(오른쪽 쇄골~손가락, 오른손 무기·AnimObjectR), `leftArm`(왼쪽, 방패·AnimObjectL). 생략하면 지금처럼 전신 동작입니다.
- `upper`는 **서 있을 때는 HKX 전신 그대로**(다리·골반까지, HKX의 다리가 숙인 상체를 받칩니다), **움직이면 다리는 이동 애니메이션**이고 상체는 HKX 자세를 지킵니다. 출발하면 속도가 오르는 데 맞춰 다리가 이동 애니메이션으로 넘어가고, 멈추면 돌아옵니다. 잠행 중에는 늘 이동 애니메이션 다리입니다.
  - 움직이는 동안 상체는 이동 애니메이션의 앞뒤·좌우 흔들림을 받지 않고, 몸통이 진행 방향으로 도는 것(옆걸음 등)만 따라 돕니다. HKX가 COM을 기울여 만든 숙임은 HKX 다리가 받치던 것이라 이동 중에는 빠지고, 척추 자체의 숙임은 남습니다.
  - 상체와 골반의 방향 차이(옆으로 튼 자세 등)는 허리 한곳이 아니라 골반→Spine→Spine1→Spine2 세 관절에 고르게 나눕니다. 가슴의 방향은 그대로입니다. 서 있을 때는 HKX 전신 그대로입니다.
  - `facing="camera"`(상체 전용): 상체가 **화면이 보는 곳**을 향합니다. 활·석궁·마법 조준처럼 목표를 가리키는 동작에 씁니다.
    - 재생하는 동안 플레이어 몸이 카메라 방향으로 돕니다(초당 약 340°). 아직 따라가지 못한 차이와 옆걸음·뒷걸음으로 하체가 돈 각도는 상체가 되돌립니다(최대 90°).
    - 카메라의 위아래 각도만큼 상체가 숙거나 젖힙니다(최대 ±69°).
    - 좌우·위아래 모두 척추 세 뼈(Spine·Spine1·Spine2)가 나눠 받습니다. HKX는 플레이어 정면(+Y)을 조준하는 자세로 만듭니다.
    - 기본값 `facing="legs"`는 위의 하체 따라감입니다(차지·물약·우산처럼 자세를 몸에 싣는 동작). NPC는 `camera`여도 하체를 따라갑니다.
  - 팔(`rightArm`·`leftArm`)은 이동 애니메이션의 몸통을 따라갑니다: 입에 댄 손이 걸으며 움직이는 머리와 함께 움직입니다.
- 상체 하나와 양팔은 동시에 재생할 수 있습니다(예: 상체 팔짱 + 오른팔 손 흔들기는 오른팔이 이깁니다). 같은 부위에 새 동작이 오면(시작이든 전환이든) 지금 동작에서 새 동작으로 바로 교차합니다(새 동작의 `enter` 시간).
- 끝: 한 번 재생이면 클립 끝(`<Exit at>`), `loop="true"`면 끝내기 전환(`<Transition key end="true">`)이나 다른 전환, `maxHold`. 부위 반복 동작의 `maxHold`는 쓴 경우에만 있고, 차지 종료 전환(`on="release"`)이 있으면 쓰지 않아도 기본 3초입니다. 들어오고 빠지는 시간은 `enter`·`<Exit blend>`(기본 0.3초).
- 저절로 끝나는 경우: 공격, 막기, 무기 꺼내기·넣기, 시전, 점프·공중, 전력질주 시작, 전신 동작 시작, 래그돌·쓰러짐·가구·탈것·수영, 1인칭 전환, 죽음.
- 전력질주 중에 시작하면 전력질주를 멈추고(달리기로) 재생합니다. 전력질주 키를 계속 누르고 있어도 다시 누르기 전에는 전력질주로 돌아가지 않습니다. NPC는 전력질주 중이면 시작하지 않습니다.
- 쓸 수 있는 것: `loop`·`maxHold`, `enter`, `<Exit at blend>`, `<Do at>`(시각 기능), 시작(`<Start>`의 키·이벤트·조건 카드), 그리고 전신 동작과 같은 `<Transition>`: 키·이벤트·누르는 방식(`press`), 끝나면(`on="end"`), 차지 종료(`on="release"`), 끝내기(`end="true"`), 조건 카드. 부위 동작과 전신 동작은 서로 전환할 수 있습니다(아래 차지). HKX 주석은 다른 동작처럼 이벤트가 됩니다(AnimObject도).
- 쓸 수 없는 것: `<Pair>`, `rig`(크리처), `view="first|both"`, `otherView` `air` `attack` `block`, `<Rotate>` `<IFrame>` `<HyperArmor>` `<TimeScale>` `<Cost>` `<MoveCancel>` `<QTE>` `<Camera>`, 이동 전환(`on="move"`, 이동하면서 재생하므로), 부위 동작에 들어가거나 나오는 전환의 `blend`(도착 동작의 `enter`로 이어집니다), 페어와 부위 동작 사이의 전환, QTE 결과·`partnerTo`가 부위 동작을 가리키는 것.
- 상체 HKX에 `FootLeft`·`FootRight` 주석을 넣지 마세요. 발소리는 이동 애니메이션이 냅니다.

### 부위 동작으로 차지

차지의 세 동작(시작·차지 중·릴리즈)은 각각 부위를 따로 정합니다. 시작과 차지 중을 상체로 두면 **걸으면서 차지**하고, 릴리즈를 전신으로 두면 멈춰 서서 내지릅니다.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="WalkingCharge">
  <Start key="G" press="hold" to="ChargeStart"/>
  <Action id="ChargeStart" clip="Animations\MyMod\ChargeStart.hkx" body="upper">
    <Transition to="ChargeHold" on="end"/>
  </Action>
  <Action id="ChargeHold" clip="Animations\MyMod\ChargeHold.hkx" body="upper" loop="true" maxHold="4">
    <Transition to="ChargeRelease" on="release"/>
  </Action>
  <Action id="ChargeRelease" clip="Animations\MyMod\ChargeRelease.hkx"/>
</AnimationSet>
```

- 같은 부위로 이어지면(시작 → 차지 중) 상체가 끊기지 않고 교차합니다. 시간은 도착 동작의 `enter`(기본 0.3초)입니다.
- 부위 → 전신(차지 중 → 릴리즈): 전신 동작이 시작되고 상체는 릴리즈의 `enter` 시간 동안 빠집니다.
- 전신 → 부위: 전신 동작이 자기 `<Exit blend>`로 빠지는 동안 부위 동작이 들어옵니다.
- 다른 부위로 가면(상체 → 오른팔) 지금 부위는 빠지고 새 부위가 들어옵니다.
- `maxHold`는 시작 키를 누른 때부터 셉니다(전신 차지와 같음). 지나면 키를 뗀 것으로 보고 릴리즈합니다.
- 차지 중에 공격·막기·점프 등이 오면 부위 동작은 그대로 끝나고 릴리즈하지 않습니다.

## 크리처 동작

`<Action>`에 `rig`를 쓰면 그 크리처가 혼자 재생하는 동작이 됩니다. 예: `<Action id="Howl" rig="wolf" clip="animations\MyMod\howl.hkx">`.

- `rig`: `wolf` `bear` `sabrecat` `troll` `draugr` `werewolf` `vampirelord` 등 크리처 이름(페어의 `FindPartner rig`와 같은 목록). 생략하면 사람(`character`)입니다. 같은 폴더·골격을 쓰는 크리처는 함께 재생합니다(`wolf`와 `dog`, `draugr`와 `skeleton`): 한쪽만 원하면 종족 조건으로 거릅니다. 에디터는 HKX를 보고 이 값을 스스로 씁니다.
- `clip`: 크리처 동작은 **그 크리처 폴더 기준**입니다(늑대는 `meshes\actors\canine\`, 곰은 `meshes\actors\bear\`). 사람 동작은 지금처럼 `meshes\actors\character\` 기준입니다.
- 시작: 크리처가 보내는 이벤트로 `<Start event="…" actor="npc" to="…">`. 키·게임 조작 시작은 플레이어가 늑대인간·뱀파이어 군주일 때 그 크리처 동작을 재생합니다. 시작이 고른 동작의 `rig`가 그 액터와 다르면 시작하지 않으므로 한 파일에 사람 동작과 크리처 동작을 함께 둘 수 있습니다.
- 쓸 수 있는 것: `loop`, `enter`, `<Exit>`, 같은 `rig` 동작으로의 `<Transition>`, `<Rotate>` `<IFrame>` `<HyperArmor>` `<MoveCancel>` `<Cost>`, 시각 기능(`<Do at>`)과 이벤트 기능, 액터 공통 조건, `block`.
- 사람 동작에만 쓰는 것: `view` `otherView` `air` `attack`, `<QTE>` `<Camera>` `<CarryRelease>`, `<Do op="hide|show|fireArrow">`. 다른 스켈레톤 계열의 동작으로 가는 `<Transition>`도 쓸 수 없습니다.

### 크리처가 시작하는 페어

`<Pair>`에 `rig`를 쓰면 그 크리처가 시작하는 쪽(가해자)입니다(크리처의 킬무브). 상대는 사람만 됩니다(`FindPartner rig`를 쓰지 않거나 `character`).

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="WolfKill">
  <Start event="NPCWolfAttackA" block="NPCWolfAttackA" actor="npc" target="combatTarget" to="WolfPounce"/>
  <Pair id="WolfPounce" rig="wolf" clip="animations\MyMod\WolfPounce.hkx">
    <FindPartner range="200" angle="90"/>
    <Exit blend="0.3"/>
  </Pair>
</AnimationSet>
```

- `clip`: 사람이 시작하는 페어와 같이 **사람 폴더 기준**입니다(`meshes\actors\character\`). 두 역할이 든 HKX 하나를 양쪽이 함께 읽습니다. 역할도 같습니다: 시작하는 크리처가 `NPC` 쪽, 상대 사람이 `2_` 쪽입니다.
- 같은 폴더·골격의 크리처도 시작합니다(`wolf`면 개도).
- 쓸 수 없는 것: 크리처 동작에서 막힌 것 전부, `<Carry>`, 크리처끼리의 페어. 전환(`<Transition to>`)은 같은 스켈레톤 계열의 동작으로, `partnerTo`는 사람 동작으로만 갑니다.

## 페어

두 사람이 함께 하는 동작은 `<Pair>`로 씁니다. HKX는 두 역할(시작한 사람과 상대)이 함께 든 페어 HKX 하나입니다. 전투 중에 F를 누르면 앞에 있는 상대와 페어를 시작합니다.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="MyGrab">
  <Start key="F" gamepad="LB" cooldown="1" to="Throw">
    <Check kind="actorStatus" option="combat"/>
  </Start>
  <Pair id="Throw" clip="Animations\MyGrab\Throw.hkx">
    <FindPartner range="150" angle="60"/>
    <Exit blend="0.4"/>
  </Pair>
</AnimationSet>
```

- `<FindPartner range angle>`: 앞쪽 몇 거리·몇 도 안의 상대를 찾습니다(기본 150·60).
- 다른 모드가 보내는 이벤트로 시작하려면 `<Start event="이름" target="combatTarget" to="…">`처럼 씁니다.
- 바닐라 동작을 바꾸려면 그 이벤트로 시작하고 같은 이벤트를 `block`에 씁니다. 예: 막았을 때 `<Start event="blockHitStart" block="blockHitStart" to="…">`. 시작할 수 있을 때만 바닐라 그래프에 그 이벤트가 가지 않고 이 동작이 재생됩니다(`chance` 조건이 있으면 막지 않음). NPC도 `actor`가 NPC를 포함하면 같습니다. `<Action block="…">`은 그 동작이 재생되는 동안 막고, 키·조작 시작의 `block`은 플레이어에게만 적용됩니다.
- 재생 중인 동작에 같은 이벤트의 `<Transition event>`가 있으면 그 이벤트는 전환이 받고, 같은 이벤트의 `<Start>`는 시작하지 않습니다(키와 같은 규칙). 구간이 열리기 전이면 선입력으로 기억했다가 열릴 때 전환하고, 닫힌 뒤에는 아무것도 하지 않습니다. 예: 막기 피격 동작 안의 `<Transition event="blockHitStart" from="0.1">`로 연속 막기.
- 조건에 `subject="partner"`를 붙이면 상대를 검사합니다. 누구를 검사해도 같은 조건(`chance` `gameHour` `weather` `global` `movementDirection` `space` `ground` `heldTime`)에는 붙일 수 없습니다.
- 페어는 기본으로 3인칭에서 재생됩니다(1인칭에서 시작하면 3인칭으로 바뀌었다가 끝나면 돌아옵니다). `<Pair view="both">`이고 1인칭 HKX(`_1stPerson` 아래 같은 경로)가 있으면, 플레이어가 1인칭일 때 1인칭으로 재생합니다. `view="first"`는 1인칭에서만 시작하는 페어로, 1인칭 HKX 하나만 있으면 됩니다(페어 HKX에 두 역할이 다 들어 있어 상대도 그 파일로 재생합니다. 없으면 시작하지 않음). `both`는 3인칭 HKX와 1인칭 HKX가 둘 다 필요합니다. 운반은 3인칭만 됩니다. 재생 중 한쪽이 죽거나 래그돌이 되면 페어는 끝납니다. 단, 페어 HKX의 `2_KillActor` 주석이 상대를 죽인 경우는 바닐라 킬무브처럼 상대만 애니메이션에서 빠지고(죽어서 래그돌), 시작한 쪽은 자기 클립을 끝까지 재생합니다(1.0.8 이상). 그 뒤의 `NPCEndAnimatedCamera`·`NPCPairEnd` 같은 주석도 그대로 옵니다. 페어 안의 무적·경직 저항 등은 `<Do at op="iframeOn">`처럼 시각 기능으로 씁니다.

### 운반 래그돌

운반(`<Carry target="physics">`)으로 들린 대상이 물리로 흔들리는 방식은 들기 동작마다 파일 하나로 정합니다. 위치는 그 동작 XML 옆의 `Ragdoll\<들기 동작 id>.xml`입니다(예: `Grapple\Grapple.xml`의 `GrappleCarryLift` → `Grapple\Ragdoll\GrappleCarryLift.xml`). 게임은 대상을 들 때마다 이 파일을 다시 읽으므로, 게임을 켠 채 고쳐도 다음 들기부터 반영됩니다. 파일이 없거나 오류가 있으면 게임 원래 래그돌 값(바닐라)으로 움직입니다(오류는 로그에 남고, 검사기는 동작 XML을 검사할 때 이 파일도 검사합니다). 에디터의 운반 항목에서 만들 수 있습니다.

```xml
<?xml version="1.0" encoding="utf-8"?>
<Ragdoll rig="character" version="1">
  <Motion follow="1" gravity="4" drag="10" spin="10" inertia="10"/>
  <Body bone="NPC COM [COM ]" simulate="false"/>
  <Body bone="NPC Spine2 [Spn2]" simulate="false"/>
  <Body bone="NPC L Thigh [LThg]" simulate="true" cone="40" twist="-15 15"/>
</Ragdoll>
```

| 요소 | 속성 |
|---|---|
| `<Ragdoll>` | `rig`(들기 대상의 리그, 다르면 쓰지 않음) `version="1"` |
| `<Motion>` | `follow`(운반자 속도 변화를 몸에 바로 주는 몫 0~1) `gravity`(중력 배율 1~4) `drag`(공기 저항 0~10) `spin`(회전 저항 0~10) `inertia`(회전 관성 배율 1~10) `friction`(관절 마찰 0~100, 쓰지 않으면 바닐라) |
| `<Body bone>` | 래그돌 몸체가 붙은 뼈마다 하나. `simulate`(false면 스스로 흔들리지 않음: 위 몸체가 물리로 흔들리면 그 관절을 처음 각도로 고정해 함께 움직이고, 위가 모두 물리 끔이면 들기 자세를 따라감), 그 몸체만의 `drag` `spin` `gravity` `inertia` `friction`, 위 몸체와의 관절 제한 `cone`(0~180도) `plane` `twist`(「최소 최대」 도), 관절 축 `swingFrom`(뼈 이름). 쓰지 않은 관절 값은 바닐라 관절 그대로입니다 |

## QTE

동작 도중 화면에 QTE를 띄우고, 결과에 따라 다른 동작으로 넘어갑니다.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="MyQte">
  <Start key="H" to="Struggle"/>
  <Action id="Struggle" clip="Animations\MyQte\Struggle.hkx">
    <QTE id="Escape" at="0.5" key="E" gamepad="A" type="rapidPress" pressCount="8">
      <Success to="EscapeWin"/>
      <Failure to="EscapeLose"/>
    </QTE>
  </Action>
  <Action id="EscapeWin" clip="Animations\MyQte\Win.hkx"/>
  <Action id="EscapeLose" clip="Animations\MyQte\Lose.hkx"/>
</AnimationSet>
```

- `type`: `splitArc` `timingBar` `rotatingNeedle`(타이밍 맞추기), `holdRelease`(누르고 있다가 떼기), `rapidPress`(연타, `pressCount`), `keySequence`(키 순서).
- 결과는 `<Success>` `<Failure>` `<Perfect>`이고, 각각 `to`(다음 동작)나 `event`(보낼 이벤트)를 씁니다.
- 같은 순간에 QTE 두 개를 띄우려면 `channel`을 다르게 씁니다. 같은 채널이면 먼저 온 하나만 뜹니다.

## 키 이름

- 키보드(`key`): `A`~`Z`, `0`~`9`, `F1`~`F12`, `Space` `Enter` `Tab` `Esc` `Backspace` `LShift` `RShift` `LCtrl` `RCtrl` `LAlt` `RAlt` `CapsLock` `Up` `Down` `Left` `Right` `Home` `End` `PageUp` `PageDown` `Insert` `Delete` `Num0`~`Num9` `NumEnter` `NumPlus` `NumMinus` `NumMul` `NumDiv` `NumDot` `Minus` `Equals` `LBracket` `RBracket` `Semicolon` `Apostrophe` `Grave` `Backslash` `Comma` `Period` `Slash`, 마우스 `Mouse1`~`Mouse7` `MouseWheelUp` `MouseWheelDown`
- 게임패드(`gamepad`): `A` `B` `X` `Y` `LB` `RB` `LT` `RT` `LS` `RS` `Start` `Back` `Up` `Down` `Left` `Right`
- 게임 동작(`control`, `key`·`gamepad` 대신): `jump` `sprint` `sneak` `activate` `readyWeapon` `shout` `rightAttack` `leftAttack` `togglePOV` `run` `toggleRun` `autoMove` `wait` `favorites` `tweenMenu`. 플레이어의 키 설정을 따라가고, 그 키로 동작이 시작되면 게임의 원래 행동(실제 점프 등)은 일어나지 않습니다.

키 이름은 물리 키 기준이라 한/영 입력 상태와 상관없습니다.

### 누름 종류 (`<Start press>`, `<Transition press>`)

| `press` | 시작하는 순간 |
|---|---|
| `down` (기본) | 누르는 순간. 기다리지 않습니다 |
| `tap` | 짧게 눌렀다 뗀 순간. 같은 키에 `double`도 있으면 두 번째 누름을 기다리는 시간이 지난 뒤 |
| `double` | 빠르게 두 번 누를 때, 두 번째로 누르는 순간. 두 번째를 길게 눌러도 `double`입니다 |
| `hold` | 누른 채 기준 시간을 넘기는 순간. 떼기를 기다리지 않습니다 |

- 기준 시간은 `AnimationInjector.ini`의 `[Input]`: `fPressHoldTime`(이보다 짧으면 `tap`, 길면 `hold`, 기본 0.2초), `fPressDoubleWindow`(첫 누름을 뗀 뒤 두 번째 누름까지, 기본 0.25초).
- 키는 그 키에 걸린 종류만큼만 기다립니다. 누르는 순간 조건(이동 방향 등)을 읽어, 시작할 수 있는 종류가 없으면 기다리지 않습니다. 이동 방향은 처음 누른 순간의 값입니다.
- 같은 키의 `down` 시작을 먼저 시도하고, 그 동작이 시작되면 그 누름은 거기서 끝납니다.
- 게임 동작(`control`)의 누름 가운데 어떤 시작도 가져가지 않은 것은 게임에 돌려줍니다. 기다린 만큼 늦게 일어납니다. 예: `<Start control="jump" press="tap" to="Dodge"/>`는 짧게 누르면 닷지, 길게 누르면 원래 점프.
- 메뉴를 여는 게임 동작(`wait` `favorites` `tweenMenu`)은 누르는 순간 메뉴가 열려 `down`만 씁니다.

**전환의 누름 종류 (1.0.8 이상)**: 키 전환(`<Transition key|gamepad|control>`)에도 같은 `press`를 씁니다. 재생 중인 동작이 자기 전환 키를 시작 입력과 같은 방식으로 가립니다.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="MyDodge">
  <Start key="LAlt" press="double" to="Roll"/>
  <Action id="Roll" clip="Animations\MyDodge\Roll.hkx">
    <Transition to="Roll2" key="LAlt" press="double" from="0.3"/>
    <Transition to="Step" key="LAlt" press="tap" from="0.3"/>
    <Transition to="Step" key="LAlt" press="hold" from="0.3"/>
  </Action>
  <Action id="Roll2" clip="Animations\MyDodge\Roll2.hkx"/>
  <Action id="Step" clip="Animations\MyDodge\Step.hkx"/>
</AnimationSet>
```

- 같은 키에 종류마다 전환 하나를 둘 수 있습니다. `down`(기본)은 누르는 순간 먼저 나가므로 같은 키에서 다른 종류와 함께 쓸 수 없습니다(`tap`을 쓰세요).
- `event`·`on` 전환에는 쓰지 않습니다.
- 기다리는 시간은 시작과 같습니다: `tap`은 뗄 때(같은 키에 `double`이 있으면 두 번째 누름을 기다린 뒤), `hold`는 기준 시간, `double`은 두 번째로 누르는 순간.
- 조건 카드의 이동 방향은 처음 누른 순간의 값입니다(판정이 끝날 때 이동키를 뗐어도).
- 동작이 끝나 가는 중이거나(종료 블렌드) 판정 도중 끝나면, 그 누름은 판정된 종류로 그 키의 `<Start>`에 넘어갑니다. `tap`·`hold`를 받는 시작이 없으면 `down` 시작으로 갑니다.

## 걷기·달리기 분리 (Locomotion)

바닐라 이동은 걷기와 달리기 애니메이션을 속도로 섞습니다. 그래서 정속으로 걸을 때도 달리기가 섞이고(한손 무기 앞 약 15%, 마법 뒤 약 31%), 재생 빠르기는 캐릭터의 속도가 바뀌어도 거의 그대로라 발이 미끄러집니다. `<Locomotion>`이 있으면 방향마다 걷기 또는 달리기 하나만 재생하고, 둘은 걷기↔달리기를 바꿀 때와 멈출 때만 섞입니다. 재생 빠르기는 캐릭터의 실제 속도를 따라갑니다.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="WalkRunSplit">
  <Locomotion>
    <Tree weapon="2HW" use="own"/>
    <Tree weapon="H2H" use="own"/>
    <Except set="Sneak"/>
  </Locomotion>
</AnimationSet>
```

- `<Locomotion/>`만 적어도 모든 무기 상태·방향에 적용됩니다. 적을 숫자는 없습니다.
- 재생 빠르기 = 캐릭터의 실제 속도 ÷ 그 파일의 발 속도. 이동 속도를 바꾸는 플러그인, 속도 버프·감속, 든 장비의 무게로 속도가 달라져도 발이 땅에 붙습니다(0.1~10배 안에서).
- 파일의 발 속도는 자동으로 구합니다. 바닐라 파일(과 그것을 OAR로 바꾼 파일)은 게임에 등록된 이동 데이터를 씁니다. 전용 트리의 파일은 아래 `<Tree>`를 보세요.
- 걷기·달리기 선택은 캐릭터의 걷기/달리기 상태와 실제 속도로 합니다. 달리다 걷기로 바꾸면 속도가 걷기 속도 근처로 내려오면서 넘어가고, 멈출 때는 다리가 걷기 쪽으로 넘어가되 상체는 달리기 동작을 유지한 채 대기 자세로 넘어갑니다. 걷기와 달리기가 섞이는 동안 상체는 달리기 것입니다(Spine2 위·팔·손·머리).
- `<Except set="…"/>`(`<Locomotion>`의 자식, 세트마다 하나): 그 무기 상태는 바닐라 그대로 둡니다. `set`은 `1HM`(한손·맨손 뽑음) `2HM`(양손검) `2HW`(양손 도끼·망치) `H2H`(맨손) `Bow` `CrossBow` `Magic`(마법 준비) `MagicCast`(시전하며 이동) `Sheathed`(무기 넣음) `Sneak`(잠행), 막기 중 이동은 `Block1HM`(한손·맨손·마법) `Block2HM`(양손) `Block2HW` `BlockH2H` `BlockBow`(활·석궁) `BlockSneak`입니다. `2HW`·`Block2HW`·`H2H`·`BlockH2H`는 그 무기의 `<Tree use="own">`이 있을 때만 있습니다. 막기 중 이동은 늘 걷기만 재생합니다.
- 원래 클립은 고치지 않고 사본을 만들므로 OAR로 교체한 애니메이션을 그대로 따릅니다. 같은 그래프를 쓰는 NPC에도 적용됩니다.
- `<Tree weapon="…" use="…"/>`(`<Locomotion>`의 자식, 무기마다 하나): 바닐라에서 다른 무기의 이동 트리를 빌려 쓰는 무기에 전용 이동을 줍니다. 그런 무기는 둘입니다.
  - `weapon="2HW"`(양손 도끼·망치): 바닐라는 다리만 2HM 방향 블렌더이고 팔은 방향 없는 걷기/달리기 블렌드라, 다리의 빠르기가 바뀌면 팔과 박자가 어긋납니다. `use="2HM"`은 이동과 잠행 이동 모두 양손검(2HM) 것을 씁니다(도끼·망치를 이동 중에 대검처럼 쥡니다). `use="own"`은 2HM과 같은 구조의 2HW 전용 트리를 만듭니다. 잠행은 2HM처럼 다리는 잠행, 상체는 이 트리입니다.
  - `weapon="H2H"`(맨손): 바닐라는 한손 무기와 같은 트리를 그대로 씁니다. `use="own"`은 그 트리를 복사한 맨손 전용 트리를 만들고 맨손 상태만 그쪽을 쓰게 합니다. 한손 무기는 그대로입니다. 맨손 잠행은 무기 공통 잠행 트리라 바뀌지 않습니다.
  - `use="own"`의 파일: 전용 트리의 각 칸은 원래 파일 이름의 앞머리를 바꾼 `meshes\actors\character\animations\2HW_<이름>.hkx`(2HM 파일 기준, 예: `2HW_RunForward.hkx`) 또는 `H2H_<이름>.hkx`(1HM 파일 기준, 예: `H2H_RunForward.hkx`)가 있으면 그 파일을, 없으면 원래 파일을 재생합니다. 파일의 길이와 보폭은 자유입니다. 파일 안에 이동 거리 주석(`animmotion x y z`, Animation Motion Revolution과 같은 표기, 예: 한 주기 끝에 `animmotion 0 190.16 0`)이 있으면 마지막 주석의 거리 ÷ 그 시각을 이 파일의 발 속도로 써서, 툴에서 발이 미끄러지지 않게 만든 걸음이 게임에서도 그대로 맞습니다. 주석이 없으면 원래 파일의 발 속도를 씁니다(원래 파일과 걸음이 같다고 보고 재생합니다). Animation Motion Revolution은 설치돼 있지 않아도 됩니다. 발 동기와 발소리는 파일 안의 `FootLeft`·`FootRight` 주석을 쓰므로 파일마다 두 주석이 모두 있어야 합니다.
  - 맨손(`weapon="H2H"`)의 `use="own"`은 움직이는 동안 팔도 이동 애니메이션을 따르게 합니다. 바닐라의 맨손은 다리·몸통만 이동 애니메이션이고 팔은 고정 자세(`H2H_LocoMotionPose.hkx`)입니다. 서 있을 때의 맨손 자세, 질주, 1인칭, 막기 이동은 그대로입니다. `H2H_` 파일이 없는 방향은 한손 무기 애니메이션이 팔까지 재생되므로, 맨손 전용 트리를 쓰는 모드는 쓰는 방향의 파일을 모두 넣는 것이 좋습니다.
  - 막기 중 이동에도 같은 트리를 만들어, 그 무기가 원래 무기의 다리 대신 이 파일로 걷습니다(막기 이동은 하체만 쓰고 상체는 막기 자세입니다).
- 선언은 게임 전체에 **한 파일**만 씁니다: `SKSE\Plugins\AnimationInjector\Locomotion\Locomotion.xml`(대소문자 무관). 이 파일에는 `<Locomotion>`만 둡니다(동작·이벤트 등록과 함께 있으면 파일 전체를 건너뜁니다). 다른 XML에 있는 `<Locomotion>`은 읽지 않고 로그에 경고를 남깁니다.
- 두 모드가 넣으면 같은 경로의 파일이 겹치므로, 모드 관리자(MO2 등)의 우선순위가 높은 모드의 파일 하나가 통째로 쓰입니다. 전용 트리의 HKX도 같은 우선순위를 따릅니다. `AnimationSet`의 `id`에는 모드 이름을 적습니다(로그에 어느 모드의 파일인지 나옵니다).
- 바닐라와 비교: `AnimationInjector.ini`의 `[Debug] iLadderToggleKey`에 키를 지정하면 게임 안에서 켜고 끌 수 있습니다. 끄면 전용 트리(`<Tree use="own">`)도 바닐라 트리로 돌아가 전용 파일이 재생되지 않습니다.

### 자세 이동 세트 (Stance)

활 조준·차지처럼 상체를 옆으로 튼 부위 동작(`body="upper"`)은 정면용 걷기·달리기 위에서 허리가 꺾여 보입니다. `<Stance>`는 그런 동작이 재생되는 동안 하체를 그 자세에 맞춘 전용 걷기·달리기로 바꿉니다.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="WalkRunSplit">
  <Locomotion>
    <Stance side="right" from="25" full="45" walk="Animations\MyBow\Walk" run="Animations\MyBow\Run"/>
  </Locomotion>
</AnimationSet>
```

- 언제: 상체 부위 동작이 재생되는 동안, 그 HKX가 가슴을 정면에서 `side` 쪽으로 튼 각도(런타임이 매 프레임 잼)가 `from`도부터 섞이기 시작해 `full`도에서 전용 하체 100%가 됩니다(기본 25·45). 동작의 들어가기·빠지기와 함께 섞입니다. 팔 부위 동작, 전신 동작, 잠행, 막기 중 이동에는 쓰지 않습니다.
- `side`: `right` 또는 `left`, 쪽마다 하나.
- 파일: `walk`·`run`은 방향 이름 앞까지의 경로입니다. 뒤에 `Forward` `ForwardLeft` `ForwardRight` `Left` `Right` `Backward` `BackwardLeft` `BackwardRight`와 `.hkx`를 붙여 찾습니다(예: `Animations\MyBow\WalkForward.hkx`). 없는 방향은 그 방향의 원래 걷기·달리기를 씁니다. 둘 중 하나만 써도 됩니다.
- 무기와 상관없이 같은 세트를 씁니다(전용 트리 `<Tree use="own">`의 무기 포함).
- HKX 만드는 법(전용 트리 파일과 같음): **한 주기**(왼발·오른발 한 번씩)로 만들고, `FootLeft`·`FootRight` 주석을 하나씩, 이동 거리 주석(`animmotion`)을 넣습니다. 이동 거리는 디딘 발이 뒤로 밀리는 속도에 맞춥니다(몸통 이동량이 발보다 크면 발이 미끄러집니다). 주기가 두 번 든 파일은 발 동기가 맞지 않아 걸음마다 튑니다.
- 바닐라 파일을 지정하면 그 파일을 재생하는 바닐라 클립의 발 트리거와 이동 속도를 씁니다(바닐라 값이 발과 맞지 않는 파일도 있습니다. 예: `bowdrawn_walk*`).

### 반대 방향 전환

- 이동 사다리가 켜져 있으면 반대 방향으로 바꿀 때(앞↔뒤, 좌↔우) 이동 블렌드가 그 순간의 자세에서 0.2초 동안 교차합니다. 따로 쓸 것은 없습니다.
- **전환 동작**: 플레이어가 한 방향으로 달리다(달리기·전력질주, 잠행 아님) 반대쪽 키를 누르면 런타임이 이벤트를 보냅니다. 오른쪽→왼쪽은 `AI_Turn_RightToLeft`, 왼쪽→오른쪽은 `AI_Turn_LeftToRight`, 앞→뒤는 `AI_Turn_ForwardToBack`, 뒤→앞은 `AI_Turn_BackToForward`. 키를 누르는 순간(엔진이 감속을 시작하기 전)이라 HKX가 멈칫하는 브레이크부터 반대로 밀어내는 출발까지 맡습니다. 이 이벤트로 시작하는 동작은 `<Locomotion>`과 다른 XML에 둡니다.

```xml
<?xml version="1.0" encoding="utf-8"?>
<AnimationSet version="6" id="PivotTurn">
  <Start event="AI_Turn_RightToLeft" actor="player" to="PivotLeft"/>
  <Start event="AI_Turn_LeftToRight" actor="player" to="PivotRight"/>
  <Action id="PivotLeft" clip="Animations\PivotTurn\PivotRightToLeft.hkx" enter="0.12">
    <Exit blend="0.2"/>
  </Action>
  <Action id="PivotRight" clip="Animations\PivotTurn\PivotLeftToRight.hkx" enter="0.12">
    <Exit blend="0.2"/>
  </Action>
</AnimationSet>
```

  - 속도: 앞·옆은 250 이상, 뒤로 달리기는 160 이상(뒤로 달리기 약 200, 뒤로 걷기 약 80). 걷는 중에는 보내지 않습니다.
  - 좌우는 옆으로 달릴 때만, 앞뒤는 키 방향대로 몸이 움직일 때만 보냅니다(뒤로 달리다 앞, 앞으로 달리다 뒤로 물러남). 캐릭터가 진행 방향을 보고 달리는 경우(True Directional Movement의 directional 설정 등)는 몸을 돌리는 것이라 보내지 않습니다.
  - 이전 키를 뗀 뒤 0.15초 안에 반대 키를 눌러야 합니다. 0.4초 안에는 다시 보내지 않습니다.
  - `enter`를 꼭 씁니다(0.1초 안팎). 달리던 자세에서 바로 들어가면 발 위치에 따라 한 프레임에 크게 튑니다(0.12초로 한 프레임 이동 68~135 → 16~21).
  - HKX: 반대쪽으로 달리던 자세에서 시작해 반대 방향 달리기 자세로 끝나게 만들고, 이동 거리(루트 이동)를 넣습니다. 이 동작이 끝나면 이동 애니메이션으로 `<Exit blend>` 동안 넘어갑니다.

## 검사하기

Animation Injector Editor 배포 파일에 든 `AnimationInjectorXmlCheck.exe`로 게임 밖에서 검사할 수 있습니다. 게임과 같은 규칙으로 읽습니다.

```
AnimationInjectorXmlCheck.exe "경로\MyMod.xml"
```

`Valid: …`가 나오면 게임이 읽을 수 있는 파일입니다. 오류가 있으면 어느 요소(예: `/AnimationSet/Action/MoveCancel`)의 무엇이 틀렸는지 나옵니다. 게임에서 읽히지 않은 파일은 `AnimationInjector.log`에 같은 이유가 남습니다.

동작 `id`는 파일(`AnimationSet id`)마다 따로라서, 여러 파일에 같은 `id`가 있어도 서로 덮어쓰지 않습니다. 두 파일이 같은 `AnimationSet id`를 쓰면 파일 경로 순으로 뒤 파일의 동작을 따로 다루고 로그에 경고합니다.
