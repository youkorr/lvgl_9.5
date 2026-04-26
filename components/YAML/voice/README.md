# Voice assistant Lottie assets

Companion JSON files for the `va_orb` widget rendered on `orb_page`.

## Layout

| Path on this repo | Path on the ESP32 SD card |
|---|---|
| `components/YAML/voice/voice_assistant.json` | `/sdcard/voice/voice_assistant.json` |

The repo mirror lets the `lottie:` Python validator auto-extract markers
at compile time (see `_find_local_lottie` in
`components/lvgl/widgets/lottie.py`) — without it, you would have to
declare every marker by hand under `markers:` in the YAML.

## Why the file is not committed verbatim

The original `voice_assistant.json` is ~100 KB of LottieFiles output.
Pasting it through chat introduced too many opportunities for subtle
corruption (smart quotes, line-ending swaps, etc.), so the canonical
copy stays on your SD card. Drop it here yourself:

```
cp /sdcard/voice/voice_assistant.json components/YAML/voice/
```

Then apply the one-shot tweak that makes the orb idle by default:

```
python3 components/YAML/voice/patch_default_segment.py \
        components/YAML/voice/voice_assistant.json idle
```

The script edits the composition out-point (`op`) so ThorVG only loops
the chosen marker by default. Other markers remain available via
`lvgl.lottie.play_marker:`.
