# Key Viewer

Geometry Dash 2.2081 · Geode 5.9.0 · v0.9.3

## 한국어

일시정지 메뉴에서 **키 뷰어**를 열고 캔버스에서 키를 선택하세요. **키** 탭의 선택 목록에는 현재 레벨에서 숨겨진 방향키도 나옵니다. 키를 끌면 위치를 바꾸고, 손잡이를 끌면 크기를 바꿉니다. **배치**를 누르면 레벨 위에서 위치를 조절할 수 있습니다.

- **키** 탭에서 입력, 이름, 배경색, 눌림 색, 테두리, 불투명도와 카운터를 선택한 키에 설정합니다.
- **노트** 탭에서는 선택한 키의 속도, 너비, 길이 배율, 표시 높이, 그라데이션, 발광과 테두리를 조절합니다.
- 색상 견본을 누르면 컬러 피커가 열립니다. 색상환과 명도 영역에서 색을 고르세요.
- **키 초기화**를 누르면 선택한 키가 기본 스타일을 다시 따릅니다. 입력과 배치 좌표는 유지됩니다. **프리셋**에서 현재 스타일을 이름 붙여 저장하고, 적용을 한 단계 되돌릴 수 있습니다. 키 이름, 개수, 매핑, 위치와 크기는 유지됩니다.
- **화면 → 언어**에서 한국어와 영어를 바꿉니다. 플레이 중 통계 언어는 다음 레벨 입장 시 적용됩니다. 설정 목록은 마우스 휠이나 오른쪽 가장자리 드래그로 스크롤합니다. 배치 모드에서 휠을 돌리면 오버레이 전체가 위아래로 움직입니다.

**키보드 키 지정**을 누른 뒤 물리 키를 누르면 그 입력을 표시합니다. 게임 내 동작에 다시 연결하려면 **게임 동작으로 전환**을 누르세요. X/Y와 W/H는 각 값을 눌러 직접 입력합니다. **노트 → Y축 자동 보정**을 켜면 화면 위쪽 공간에 맞춰 타임라인 길이를 줄이고, 위쪽 외부 카운터를 피합니다. **통계** 탭에서는 선택한 키의 카운터 배치와 외곽선, 글꼴, 동작을 설정할 수 있습니다.

**키 → 키 이름 입력**에서 사용할 이름을 직접 적습니다. **화면** 탭의 가로·세로 줄맞춤은 선택한 키를 기준으로 보이는 키를 정렬합니다. 균등 배치는 **키 사이 간격**을 적용합니다. **노트 → 네온 그라데이션 적용**을 누르면 아래쪽의 노트 색에서 밝은 위쪽 색으로 이어지며 테두리와 발광이 표시됩니다.

## 노트와 통계 박스

노트의 길이는 누른 시간에 비례합니다. **노트 → 누른 시간 길이 배율**에서 표시 배율을 조절하세요. **통계 → 꾸밀 카운터**에서 합계, 초당 입력, 최고 KPS를 각각 골라 배치, 제목, 언어, 색과 글꼴을 편집합니다.

**프리셋** 탭에는 사진 속 노트를 참고한 **빨간 네온**, **청록 유리**, **빨강 흰색 끝** 스타일이 있습니다. 노트 색, 위쪽 색, 그라데이션 모양, 테두리와 발광을 따로 조절할 수 있습니다.

## English

Open **Key Viewer** from the pause menu. Select a key on the canvas or from the Key tab. Individual key and note styles inherit their defaults until edited. Open the color picker from a color swatch to choose any 24-bit RGB color. Use **Reset** to restore the selected key's style. **Sets** saves and applies visual styles while preserving names, bindings, and geometry. Switch languages in **View → Language**.

## Build

With the Geode SDK and CLI configured, run `geode build` in this directory. The GitHub Actions workflow builds separate archives for Windows, macOS, iOS and Android (32-bit and 64-bit).
