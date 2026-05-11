// Board catalog for ncaseonetwo.xyz
// Each entry produces a starter ESPHome YAML.
window.BOARDS = [
  // ─── Espressif official DevKits ────────────────────────────────
  { id: "esp32-devkitc-v4",   brand: "Espressif", name: "ESP32 DevKitC v4",      chip: "esp32",       flash: "4MB",  psram: "—",      framework: "arduino", tag: "Classic" },
  { id: "esp32-s3-devkitc-1", brand: "Espressif", name: "ESP32-S3 DevKitC-1",    chip: "esp32-s3",    flash: "8MB",  psram: "8MB OPI",framework: "esp-idf", tag: "Popular" },
  { id: "esp32-s3-devkitm-1", brand: "Espressif", name: "ESP32-S3 DevKitM-1",    chip: "esp32-s3",    flash: "8MB",  psram: "—",      framework: "esp-idf", tag: "" },
  { id: "esp32-c3-devkitm-1", brand: "Espressif", name: "ESP32-C3 DevKitM-1",    chip: "esp32-c3",    flash: "4MB",  psram: "—",      framework: "esp-idf", tag: "RISC-V" },
  { id: "esp32-c6-devkitc-1", brand: "Espressif", name: "ESP32-C6 DevKitC-1",    chip: "esp32-c6",    flash: "8MB",  psram: "—",      framework: "esp-idf", tag: "Wi-Fi 6" },
  { id: "esp32-h2-devkitm-1", brand: "Espressif", name: "ESP32-H2 DevKitM-1",    chip: "esp32-h2",    flash: "4MB",  psram: "—",      framework: "esp-idf", tag: "Thread" },
  { id: "esp32-p4-function-ev",    brand: "Espressif", name: "ESP32-P4 Function EV (32MB PSRAM)", chip: "esp32-p4", flash: "16MB", psram: "32MB", framework: "esp-idf", tag: "Flagship", esphomeBoard: "esp32-p4-evboard" },
  { id: "esp32-p4-function-ev-r16",brand: "Espressif", name: "ESP32-P4 Function EV (16MB PSRAM)", chip: "esp32-p4", flash: "16MB", psram: "16MB", framework: "esp-idf", tag: "Flagship", esphomeBoard: "esp32-p4-evboard" },

  // ─── M5Stack ───────────────────────────────────────────────────
  { id: "m5stack-core2",      brand: "M5Stack",   name: "Core2",                 chip: "esp32",       flash: "16MB", psram: "8MB",    framework: "arduino", tag: "Touch" },
  { id: "m5stack-cores3",     brand: "M5Stack",   name: "CoreS3",                chip: "esp32-s3",    flash: "16MB", psram: "8MB",    framework: "esp-idf", tag: "" },
  { id: "m5stack-atoms3",     brand: "M5Stack",   name: "AtomS3",                chip: "esp32-s3",    flash: "8MB",  psram: "—",      framework: "esp-idf", tag: "Tiny" },
  { id: "m5stack-stamps3",    brand: "M5Stack",   name: "StampS3",               chip: "esp32-s3",    flash: "8MB",  psram: "—",      framework: "esp-idf", tag: "" },

  // ─── LilyGO ────────────────────────────────────────────────────
  { id: "lilygo-t-display-s3",     brand: "LilyGO", name: "T-Display S3",        chip: "esp32-s3",    flash: "16MB", psram: "8MB",    framework: "arduino", tag: "1.9″ LCD" },
  { id: "lilygo-t-display-s3-pro", brand: "LilyGO", name: "T-Display S3 Pro",    chip: "esp32-s3",    flash: "16MB", psram: "8MB",    framework: "arduino", tag: "2.33″" },
  { id: "lilygo-t-watch-s3",       brand: "LilyGO", name: "T-Watch S3",          chip: "esp32-s3",    flash: "16MB", psram: "8MB",    framework: "arduino", tag: "Wearable" },
  { id: "lilygo-t-rgb",            brand: "LilyGO", name: "T-RGB 2.1″",          chip: "esp32-s3",    flash: "16MB", psram: "8MB",    framework: "arduino", tag: "Round" },

  // ─── Waveshare ─────────────────────────────────────────────────
  { id: "ws-esp32-s3-touch-lcd-7", brand: "Waveshare", name: "ESP32-S3 Touch LCD 7\"", chip: "esp32-s3", flash: "16MB", psram: "8MB OPI", framework: "esp-idf", tag: "7″ RGB" },
  { id: "ws-esp32-s3-touch-lcd-43",brand: "Waveshare", name: "ESP32-S3 Touch LCD 4.3\"",chip: "esp32-s3", flash: "16MB", psram: "8MB OPI", framework: "esp-idf", tag: "4.3″" },
  { id: "ws-esp32-p4-nano",        brand: "Waveshare", name: "ESP32-P4 Nano (32MB PSRAM)",     chip: "esp32-p4",  flash: "16MB", psram: "32MB", framework: "esp-idf", tag: "MIPI-DSI", esphomeBoard: "esp32-p4-evboard" },
  { id: "ws-esp32-p4-nano-r16",    brand: "Waveshare", name: "ESP32-P4 Nano (16MB PSRAM)",     chip: "esp32-p4",  flash: "16MB", psram: "16MB", framework: "esp-idf", tag: "MIPI-DSI", esphomeBoard: "esp32-p4-evboard" },
  { id: "ws-esp32-p4-86-panel",    brand: "Waveshare", name: "ESP32-P4 86 Panel (32MB PSRAM)", chip: "esp32-p4",  flash: "16MB", psram: "32MB", framework: "esp-idf", tag: "",         esphomeBoard: "esp32-p4-evboard" },
  { id: "ws-esp32-p4-86-panel-r16",brand: "Waveshare", name: "ESP32-P4 86 Panel (16MB PSRAM)", chip: "esp32-p4",  flash: "16MB", psram: "16MB", framework: "esp-idf", tag: "",         esphomeBoard: "esp32-p4-evboard" },

  // ─── Seeed Studio ──────────────────────────────────────────────
  { id: "seeed-xiao-esp32-s3",   brand: "Seeed",  name: "XIAO ESP32-S3",         chip: "esp32-s3",    flash: "8MB",  psram: "8MB",    framework: "esp-idf", tag: "Thumb-size" },
  { id: "seeed-xiao-esp32-c3",   brand: "Seeed",  name: "XIAO ESP32-C3",         chip: "esp32-c3",    flash: "4MB",  psram: "—",      framework: "esp-idf", tag: "" },
  { id: "seeed-xiao-esp32-c6",   brand: "Seeed",  name: "XIAO ESP32-C6",         chip: "esp32-c6",    flash: "4MB",  psram: "—",      framework: "esp-idf", tag: "" },

  // ─── Adafruit ──────────────────────────────────────────────────
  { id: "adafruit-feather-s3",   brand: "Adafruit", name: "Feather ESP32-S3",    chip: "esp32-s3",    flash: "8MB",  psram: "2MB",    framework: "arduino", tag: "" },
  { id: "adafruit-qt-py-s3",     brand: "Adafruit", name: "QT Py ESP32-S3",      chip: "esp32-s3",    flash: "8MB",  psram: "2MB",    framework: "arduino", tag: "Tiny" },

  // ─── Unexpected Maker ──────────────────────────────────────────
  { id: "um-feathers3",          brand: "UM",     name: "FeatherS3",             chip: "esp32-s3",    flash: "16MB", psram: "8MB",    framework: "arduino", tag: "" },
  { id: "um-tinys3",             brand: "UM",     name: "TinyS3",                chip: "esp32-s3",    flash: "8MB",  psram: "8MB",    framework: "arduino", tag: "Tiny" },
  { id: "um-promicro-s3",        brand: "UM",     name: "PROS3",                 chip: "esp32-s3",    flash: "16MB",  psram: "8MB",   framework: "arduino", tag: "" },

  // ─── Olimex / Wemos / Generic ──────────────────────────────────
  { id: "wemos-lolin-s3",        brand: "Wemos",  name: "LOLIN S3",              chip: "esp32-s3",    flash: "16MB", psram: "8MB",    framework: "arduino", tag: "" },
  { id: "wemos-lolin-s3-mini",   brand: "Wemos",  name: "LOLIN S3 Mini",         chip: "esp32-s3",    flash: "4MB",  psram: "2MB",    framework: "arduino", tag: "Mini" },
  { id: "generic-esp32-s3-n16r8",brand: "Generic",name: "ESP32-S3 N16R8",        chip: "esp32-s3",    flash: "16MB", psram: "8MB OPI",framework: "esp-idf", tag: "Bare" },
  { id: "generic-esp32-p4-n16r32",brand:"Generic",name: "ESP32-P4 N16R32",       chip: "esp32-p4",    flash: "16MB", psram: "32MB", framework: "esp-idf", tag: "Bare", esphomeBoard: "esp32-p4-evboard" },
  { id: "generic-esp32-p4-n16r16",brand:"Generic",name: "ESP32-P4 N16R16",       chip: "esp32-p4",    flash: "16MB", psram: "16MB", framework: "esp-idf", tag: "Bare", esphomeBoard: "esp32-p4-evboard" },
];

window.CHIP_LABEL = {
  "esp32":     "ESP32",
  "esp32-s2":  "ESP32-S2",
  "esp32-s3":  "ESP32-S3",
  "esp32-c3":  "ESP32-C3",
  "esp32-c6":  "ESP32-C6",
  "esp32-h2":  "ESP32-H2",
  "esp32-p4":  "ESP32-P4",
};

window.BOARD_TEMPLATE = function(b, opts) {
  opts = opts || {};
  const dev      = opts.esphomeBranch === "dev";
  const repoSlug = opts.repoSlug || "youkorr/lvgl_9.5"; // fallback
  // Some boards in the catalogue have a marketing-friendly id that ESPHome /
  // PlatformIO don't recognise. For those, set `esphomeBoard` on the entry to
  // override what lands in `board:` while keeping the descriptive id for the
  // UI / cache key. Example: every ESP32-P4 variant compiles against the
  // single 'esp32-p4-evboard' board.
  const boardName = b.esphomeBoard || b.id;
  return `# ─── Generated by ncaseonetwo.xyz ───
# Compile target: ESPHome ${dev ? "DEV" : "STABLE"} · ${b.brand} ${b.name}

esphome:
  name: ${b.id}
  friendly_name: ${b.brand} ${b.name}

esp32:
  board: ${boardName}
  variant: ${b.chip}
  framework:
    type: ${b.framework}

external_components:
  - source: github://${repoSlug}
    refresh: 0s

logger:
api:
ota:
  - platform: esphome

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
`;
};
