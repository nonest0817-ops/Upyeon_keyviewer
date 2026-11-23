# v1.0.9

- Keep a single note continuous throughout a hold and let its tail leave the track after release.
- Draw smooth key and statistics box shadows with editable shadow controls for the Total and KPS boxes.

# v1.0.8

- Copy and paste selected keys from the keyboard or the editor.
- Add independently editable third gradient stops and a Glacier Cyan preset based on the reference.
- Render one note per hold and let it leave the track without generating another segment.

# v1.0.7

- Select multiple canvas items with a drag rectangle and move them together.
- Feed new note segments while a key is held as earlier segments travel out.
- Keep or reset per-key press counts independently of the Total box on level exit.

# v1.0.6

- Undo recent editor changes with Ctrl+Z.
- Feather note glow in one continuous mesh and let held notes travel out of the track.
- Select, drag, and resize the background photo directly on the canvas.
- Add a setting to keep the Total value between levels.

# v1.0.5

- Render a continuous note gradient with vertex colors, and clip notes at the end of the track as they exit.
- Add the Cyan Flow preset based on the reference video.
- Keep editable key names blank when saved empty.
- Add a resizable PNG or JPEG background behind the keys with position and opacity controls.
- Refresh the light and dark editor surfaces and navigation.

# v1.0.4

- Use Geode keyboard events to capture and play exclusively mapped physical keys.
- Add per-key Player 2 filtering and preserve mappings when applying presets.
- Remove notes as their leading edge reaches the end of the track.
- Drag the two gradient stops and configure darkness or brightness along the track.

# v1.0.3

- Add a canvas zoom panel and interactive minimap.
- Add per-key Note and Advanced settings with explicit Save and Cancel.
- Add configurable drop shadows and raised key faces.
- Draw a full-width note gradient and give both color stops direct opacity controls.

# v1.0.2

- Use the mouse wheel over number fields. Touch users can tap either half.
- Keep the gradient body visible while preserving the color and alpha of the fading tip.
- Restore selected-key presets and add six gradient styles.

# v1.0.1

- Simplify the editor with light and dark themes and exact numeric fields.
- Use compact key and statistics tiles. Select, move and resize statistics directly on the canvas.
- Edit bottom and fading tip colors and opacity in a visual gradient picker.
- Remove the preset interface and decorative key and note effects.

# v1.0.0

- Refresh the editor with an updated palette, color swatches, and live gameplay tile previews.
- Add Alloy, Glass and Edge finishes, input sweeps, shockwaves, sparks, key travel and hold meters.
- Add note light strips and optional activity meters for stat tiles.
- Add six Studio presets and retain the three photo-inspired presets.

# v0.9.4

- Open metric settings with direct buttons or by clicking a preview tile.
- Capture keyboard bindings before the pause menu receives the key.
- Save and apply presets per selected key, including photo presets and legacy styles.

# v0.9.3

- Scale note length to input hold duration.
- Configure independent Total, KPS and Peak KPS tiles.
- Save named, style-only presets and undo the last application.
- Include three photo-inspired presets with gradient profiles and adjustable rims.

# v0.9.2

- Type individual key names, including Hangul.
- Align visible keys horizontally or vertically and space them evenly.
- Choose colors with a visual picker.
- Apply a neon gradient with a bright rim and colored glow.

# v0.9.1

- Scroll the editor with the mouse wheel and move the overlay vertically in Map mode.
- Save preset changes immediately, restore viewer visibility, and report invalid presets.
- Bind physical keyboard keys and type exact X/Y and W/H values.
- Customize key text, font, color, note auto-Y and counter placement, fill, outline and motion per key.

# v0.9.0

- Korean and English editor with bundled Korean UI glyphs.
- Full 24-bit color editing and per-key note, glow and counter settings saved in presets.
- Independent P1/P2 holds and counters on shared lanes.
- Avoid hidden note creation and skip hidden lane animation.

# v0.8.0

- Smoother note and box gradients (short notes no longer split into two flat colors)
- The editor canvas now fits all keys on screen, so keys moved in Map mode are always visible in the window
- Map mode only moves keys; resizing is available in the window mode
- New note styling settings: opacity, fade, roundness, border and border color, glow strength, glow size and an optional custom glow color

# v0.7.0

- Keys can now be added and deleted in the editor; every key has its own input, label, position, size, colors and border
- Select a key to get a blue outline with handles: drag it to move, drag the handles to resize
- "Map" mode shows the level faintly behind the keys so you can place them on the real screen
- Built-in presets replaced by user presets: save the current look and keys under a name, load, overwrite or delete them
- Fixed keys not reacting in dual and 2-player mode (2P presses now reach the 1P key when no 2P key exists)

# v0.6.0

- New in-game editor window: open the pause menu and press "Key Viewer"
- Select a key, drag it around, and edit its offset, size, rounding, border and colors
- Note, counter and view tabs, presets, P2 / show / press-preview switches
- New "Show Key Viewer" setting

# v0.5.0

- Notes, glow and boxes now use smooth images instead of stacked shapes, so no more ring or band lines
- New Gradient Colors setting (notes, boxes or both) with a custom end color
- Default corner roundness changed to 16

# v0.4.1

- Softer glow without visible ring lines
- Smoother note gradient
- Settings names and descriptions are now in English so they show up in the game's font

# v0.4.0

- Square key tiles by default (new "jump box width" setting)
- Notes now match the tile width, with new note thickness and minimum length settings
- Smoother note gradient (no visible bands)
- New bold font (Inter Bold) for labels, counters and stats; long labels shrink to fit

# v0.3.0

- New dark tile design: dark rounded tiles that light up when pressed, auto text contrast
- Notes are now rounded bars with a soft glow that fade as they rise
- Per-key label and accent color settings
- New settings: corner roundness, box gap, tile opacity, note glow
- New presets: Dark, Neon, Rhythm, Light, Minimal

# v0.2.0

- Add drag-to-move while the pause menu is open
- Add press glow
- Add built-in presets
- Add separate 2P row (experimental)

# v0.1.0

- Initial release: key boxes, note timeline, counters, KPS, settings
