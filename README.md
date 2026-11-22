# Key Viewer

Geometry Dash 2.2081 · Geode 5.9.0 · v0.9.1

## 한국어

일시정지 메뉴에서 **키 뷰어**를 열고 캔버스에서 키를 선택하세요. **키** 탭의 선택 목록에는 현재 레벨에서 숨겨진 방향키도 나옵니다. 키를 끌면 위치를 바꾸고, 손잡이를 끌면 크기를 바꿉니다. **배치**를 누르면 레벨 위에서 위치를 조절할 수 있습니다.

- **키** 탭에서 입력, 이름, 배경색, 눌림 색, 테두리, 불투명도와 카운터를 선택한 키에 설정합니다.
- **노트** 탭에서는 선택한 키의 속도, 너비, 최소 길이, 표시 높이, 그라데이션, 발광과 테두리를 조절합니다.
- 색상표 아래의 여섯 자리를 눌러 원하는 HEX 색을 만드세요. 왼쪽 두 자리는 빨강, 가운데 두 자리는 초록, 오른쪽 두 자리는 파랑입니다. 각 자리는 0부터 F까지 순환합니다. 예를 들어 청록색은 #20C9B0입니다.
- **키 초기화**를 누르면 선택한 키가 기본 스타일을 다시 따릅니다. 입력과 배치 좌표는 유지됩니다. **프리셋**에서 현재 배치와 키별 스타일을 함께 저장하고 불러옵니다.
- **화면 → 언어**에서 한국어와 영어를 바꿉니다. 플레이 중 통계 언어는 다음 레벨 입장 시 적용됩니다. 설정 목록은 마우스 휠이나 오른쪽 가장자리 드래그로 스크롤합니다. 배치 모드에서 휠을 돌리면 오버레이 전체가 위아래로 움직입니다.

**키보드 키 지정**을 누른 뒤 물리 키를 누르면 그 입력을 표시합니다. 게임 내 동작에 다시 연결하려면 **게임 동작으로 전환**을 누르세요. X/Y와 W/H는 각 값을 눌러 직접 입력합니다. **노트 → Y축 자동 보정**을 켜면 화면 위쪽 공간에 맞춰 타임라인 길이를 줄이고, 위쪽 외부 카운터를 피합니다. **통계** 탭에서는 선택한 키의 카운터 배치와 외곽선, 글꼴, 동작을 설정할 수 있습니다.

## English

Open **Key Viewer** from the pause menu. Select a key on the canvas or from the Key tab. Individual key and note styles inherit their defaults until edited. Tap each of the six hexadecimal digits below a palette to choose any 24-bit RGB color. Use **Reset** to restore the selected key's style. **Sets** saves the layout and individual overrides together. Switch languages in **View → Language**.

## Build

With the Geode SDK and CLI configured, run `geode build` in this directory. The GitHub Actions workflow builds separate archives for Windows, macOS, iOS and Android (32-bit and 64-bit).
