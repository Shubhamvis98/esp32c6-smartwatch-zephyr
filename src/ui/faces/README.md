# Watchfaces

There are two kinds of watchface:

| | Code face | Custom face |
|---|---|---|
| What it is | A C file using LVGL directly | `face.json` + images in `/data/faces/<id>/` |
| How it gets on the watch | Build and flash | Upload over Wi-Fi, no reflash |
| Can do | Anything LVGL can | The elements listed below |

Both appear in **Settings → Watchface**. The watch only builds the selected face.
If a face fails to load, the watch shows Digital instead.

## Custom faces (no reflash)

1. On the watch, connect to Wi-Fi from **Settings → Wi-Fi**.
2. Open **Settings → Watchface → Upload faces**. The screen shows an address such as
   `http://192.168.0.42`. The server only runs while this screen is open.
3. On a phone or PC on the same Wi-Fi, do either of these:
   - Open that address in a browser. From the page you can upload a face folder,
     edit `face.json` (a template is included), switch faces and delete faces.
   - Use the command line:
     ```
     pip install pillow
     python3 tools/face_push.py 192.168.0.42 faces/neon        # upload and show it
     python3 tools/face_push.py 192.168.0.42 --list
     python3 tools/face_push.py 192.168.0.42 --delete neon
     ```

A face folder holds `face.json` and any images. PNG and JPG images are converted to LVGL
`.bin` files during upload. In `face.json`, refer to an image by its original name, e.g.
`"hand.png"`. `faces/neon/` in this repo is an example.

### face.json

```json
{
  "name": "Neon",
  "desc": "Step ring, big time",
  "background": "#05050A",
  "background_image": "bg.png",
  "elements": [
    { "type": "arc",  "value": "steps", "size": 226, "width": 10, "color": "#FF2D95" },
    { "type": "text", "text": "{HH}:{mm}", "font": 48, "color": "#FFFFFF", "y": -18 },
    { "type": "hand", "hand": "minute", "src": "minute.png", "pivot_x": 3, "pivot_y": 92 }
  ]
}
```

The screen is 240×280. Elements are drawn in the order listed, so later elements are on top.

**Top-level fields**

- `name`, `desc`: shown in Settings.
- `background`: background colour, `"#RRGGBB"`.
- `background_image`: an image, normally 240×280.

**Fields every element has:** `type`, `x`, `y` (offset in pixels), and `align` (where `x`/`y`
are measured from). `align` is one of `center` (the default), `top`, `bottom`, `left`, `right`,
`top_left`, `top_right`, `bottom_left`, `bottom_right`.

| type | fields |
|---|---|
| `text` | `text` (may contain tokens), `font` (14 16 20 24 36 48), `color`, `width`, `text_align` (left center right), `letter_space` |
| `rect` | `w`, `h`, `color`, `radius`, `opacity` (0–255), `border_width`, `border_color` |
| `image` | `src` |
| `arc` | `value`, `size`, `width`, `color`, `bg_color`, `start_angle`, `end_angle` (degrees; 0 = 3 o'clock, clockwise), `rounded` |
| `bar` | `value`, `w`, `h`, `color`, `bg_color`, `radius` |
| `hand` | `hand` (hour minute second), `cx`, `cy` (pivot point on screen, default 120,140), plus one of the two styles below |

A `hand` can be drawn two ways:

- **Line hand:** set `length`, `tail`, `width` and `color`.
- **Image hand:** set `src` to an image drawn pointing at 12 o'clock. `pivot_x` and `pivot_y`
  give the point in the image it rotates around (default: bottom centre). Image hands must
  be under 48 KB because they are rotated in RAM.

**Values** for `arc` and `bar`: `steps` (progress towards the daily goal), `battery`,
`seconds`, `minutes`, `hours`.

**Text tokens:**

| Token | Shows |
|---|---|
| `{HH}`, `{H}` | Hour, following the 12/24 h setting (`{HH}` has a leading zero) |
| `{mm}`, `{ss}` | Minutes, seconds |
| `{AMPM}` | AM/PM, or nothing in 24 h mode |
| `{day}`, `{ddd}`, `{DDD}` | Monday, Mon, MON |
| `{D}`, `{DD}` | Day of the month (`{DD}` has a leading zero) |
| `{month}`, `{mmm}`, `{MMM}` | January, Jan, JAN |
| `{MM}` | Month number |
| `{YYYY}` | Year |
| `{steps}`, `{goal}` | Today's steps, daily step goal |
| `{battery}` | Battery percentage |

Before the clock has been set, time tokens show `--` and hands are hidden.

**Limits**

- Face id: 1–23 characters, using only a-z, 0-9, `_` and `-`.
- Files: 400 KB each. `face.json` can be at most 16 KB.
- Up to 16 custom faces and 48 changing elements per face.
- Images up to 48 KB are kept in RAM. Larger images, such as a full-screen background
  (134 KB), are read from flash as the screen is drawn.

When a face has a problem (bad JSON, a missing image and so on), the upload page and
`face_push.py` show the error the watch reports.

## Code faces (flash)

1. Copy `face_digital.c` to `face_<name>.c`. Then:
   - Build the widgets in `create()`. Make them children of `root`.
   - Refresh them in `update()` from the `face_data_t` it receives (time, steps, battery,
     12/24 h).
   - Define a `const watchface_t g_face_<name>`, giving it a unique `id`.
2. Add the file to `SRCS` in `main/CMakeLists.txt`.
3. Add `&g_face_<name>` to `s_builtin[]` in `faces.c`.

Use the helpers in `watchface.h`, such as `face_make_label()` and `face_set_text()`.
Widgets must not be clickable, or swipes on the watchface stop working.
