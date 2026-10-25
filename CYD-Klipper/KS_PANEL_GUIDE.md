# Writing a KlipperScreen panel for the Panda Touch (LVGL 8.3)

Goal: each Sovol KlipperScreen panel (`~/projects/CYD-Klipper-CrowPanel/../` see "Sources") rebuilt in C++/LVGL with the SAME features,
layout logic, labels and behaviour. No reinterpretation: read the Python panel and port what it does.

## Sources (read-only)
- Sovol KlipperScreen: `/tmp/claude-1000/-home-tommaso-projects-manutenzione-dispositivi-sv08/4993f073-5af3-43da-9ca3-ed777bd3f3d2/scratchpad/sovol-sv08/repo/home/sovol/KlipperScreen/panels/<name>.py`
- Sovol patches (newer versions of some panels / base): `.../repo/home/sovol/patch/klipperscreen/*.py` (print.py, job_status.py, plr.py, system.py ...). If a patched version exists, it wins.
- Menu tree, option defaults: `.../KlipperScreen/ks_includes/defaults.conf`; helpers `ks_includes/screen_panel.py`, `ks_includes/KlippyGcodes.py`.
- Reference implementations already done: `src/ks/ks_move.cpp` (grid of buttons, toggles, options page), `src/ks/ks_settings.cpp` (list of rows, switch, slider).

## Screen
800x480 landscape. The panel content area is **716 x 416 px** (the action bar and title bar are drawn by the shell). Touch only:
every tappable thing is >= 56 px. Fonts: `ks_font_18` (hints), `ks_font_22` (labels), `ks_font_28` (titles/values), `ks_font_36` (big numbers).
Colours (ks_ui.h): `KS_BG`, `KS_ACTIVE`, `KS_BLUE`, `KS_ERROR`, `KS_WARNING`, `ks_color(1..4)` = orange, magenta, teal, lime.
Icons: `ks_img(parent, "name", px, color)` / `ks_button(...)` with the original KlipperScreen icon names (z-bolt set, sizes 40/56/72).

## What you write
One file per panel: `src/ks/ks_<name>.cpp` defining `void ks_panel_<name>(lv_obj_t* content)` (the declaration is in `ks_panels.h`; a weak
placeholder in `ks_stubs.cpp` is replaced automatically). Sub-pages: `ks_open("Title", init_fn)` pushes, the action-bar back button pops.
Do NOT edit shared files (`ks_ui.*`, `ks_mr.*`, `ks_widgets.*`, `ks_menu.cpp`, `ks_panels.h`, `ks_stubs.cpp`, `main.cpp`, drivers).
If you need a shared feature, implement it privately (static) in your own file and mention it in your report.

## API you have
- Widgets (`ks_widgets.h`): `ks_list`, `ks_row`, `ks_row_right`, `ks_card`, `ks_switch`, `ks_slider`, `ks_toggles`, `ks_textarea` + keyboard,
  `ks_on_state(obj, cb)` (called ~1x/s with fresh status), `ks_fmt_duration`. Also `ks_button/ks_button_sized`, `ks_label`, `ks_img`,
  `ks_toast`, `ks_confirm(title, text, ok_label, on_ok, user)` (ks_panels.h), `ks_open/ks_back/ks_home`.
- Printer status (`ks_mr.h`), thread-safe copies: `ks_f(obj, field)`, `ks_fi(obj, field, idx)`, `ks_b`, `ks_str`, `ks_has(obj)`,
  `ks_objects("heater_generic ", names, max)`, `ks_macros(names, max)`, `ks_with_status(fn, user)` for nested data (e.g. bed_mesh, print_stats).
  All printer objects are polled except configfile, mcu, tmc and gcode_macro bodies; names like `"extruder"`, `"heater_bed"`, `"fan"`, `"toolhead"`,
  `"gcode_move"`, `"print_stats"`, `"virtual_sdcard"`, `"display_status"`, `"bed_mesh"`, `"output_pin NAME"`, `"fan_generic NAME"` ...
  Anything else (configfile settings, file lists, history, spoolman, power devices, system info, webcams) via `ks_get_async("/path", cb, user)`.
- Commands: `ks_gcode("fmt", ...)` (queued, never blocks), `ks_post("/path", "{json}")`, `ks_post_async(path, body, cb, user)`.
- Binary download (thumbnails, snapshots): `ks_get_bytes_async(path, cb, user)`; the callback owns the PSRAM buffer, release it with `ks_free_bytes`.
  PNG can be shown with an `lv_img_dsc_t` of `cf = LV_IMG_CF_RAW_ALPHA` pointing at the bytes (lodepng decoder is registered).
- Persistent options: `ks_cfg_bool/ks_cfg_int` (+ setters), key names as in KlipperScreen.conf.

## Rules
1. **Never block the LVGL thread**: no `delay()`, no synchronous HTTP. Async callbacks run later in the LVGL thread; the panel may be gone by then:
   keep per-panel state in a heap struct, free it in an `LV_EVENT_DELETE` handler, and null out widget pointers there; check them in callbacks.
2. Dangerous actions go through `ks_confirm` (cancel print, restart/shutdown host, firmware restart, delete file, disable motors).
3. Memory: ~100 KB internal heap free. Big buffers (images, JSON) in PSRAM (`heap_caps_malloc(n, MALLOC_CAP_SPIRAM)`); JsonDocument from the callbacks is fine.
4. Match KlipperScreen: same labels, same options, same enable conditions (e.g. a panel hides rows for hardware that is not present: use `ks_has`/`ks_objects`).
5. While developing you may do **read-only HTTP GET** against the printer (`http://100.108.128.30:7125/...`) to learn the JSON shapes. Never send gcode, never
   POST, never heat or move anything, never flash the panel (the integrator does that).
6. Comments: only where the reason is not obvious; keep the Python panel name in the file header comment.

## Build check (isolated, so agents do not disturb each other)
```
W=/tmp/claude-1000/ks_work_<yourname>; mkdir -p $W
rsync -a --exclude=.pio ~/projects/CYD-Klipper-CrowPanel/CYD-Klipper/ $W/
mkdir -p $W/.pio; cp -al ~/projects/CYD-Klipper-CrowPanel/CYD-Klipper/.pio/libdeps $W/.pio/libdeps
cd $W && ~/.claude/skills/watchjob/scripts/watchjob.sh ks-<yourname>-build -- /home/tommaso/.platformio/penv/bin/pio run -e esp32-PANDATOUCH
~/.claude/scripts/job log ks-<yourname>-build 300 | grep -E "error:|undefined|SUCCESS|FAILED"
```
Long commands go through `watchjob` (never nohup/&). When your files compile cleanly, copy ONLY your own `src/ks/ks_<name>.cpp` files into
`~/projects/CYD-Klipper-CrowPanel/CYD-Klipper/src/ks/` (they must not collide with another agent's file names) and report.
