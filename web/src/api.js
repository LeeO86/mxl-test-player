// REST client for the player API (SPECIFICATION.md §9), display helpers and the value lists the
// API accepts (pattern, signal, format, ... names as src/*.cpp parse them).

async function request(path, { method = "GET", body, raw, text = false } = {}) {
  const headers = {};
  if (body !== undefined) headers["Content-Type"] = "application/json";
  const resp = await fetch(path, {
    method,
    headers,
    body: raw !== undefined ? raw : body === undefined ? undefined : typeof body === "string" ? body : JSON.stringify(body),
    cache: "no-store",
  });
  const data = await resp.text();
  let parsed = data;
  if (!text) {
    try {
      parsed = data ? JSON.parse(data) : null;
    } catch {
      parsed = data;
    }
  }
  if (!resp.ok) {
    const reason = parsed && typeof parsed === "object" ? parsed.error : String(parsed || "").slice(0, 200);
    const error = new Error(reason || `HTTP ${resp.status}`);
    error.status = resp.status;
    throw error;
  }
  return parsed;
}

export const api = {
  get: (path) => request(path),
  text: (path) => request(path, { text: true }),
  post: (path, body) => request(path, { method: "POST", body: body ?? {} }),
  put: (path, body) => request(path, { method: "PUT", body }),
  patch: (path, body) => request(path, { method: "PATCH", body }),
  del: (path) => request(path, { method: "DELETE" }),
  putRaw: (path, raw) => request(path, { method: "PUT", raw }),
};

/** HTTP status of a probe (/livez, /readyz) without throwing. */
export async function probe(path) {
  try {
    const resp = await fetch(path, { cache: "no-store" });
    return resp.status;
  } catch {
    return 0;
  }
}

export function fmtBytes(n) {
  if (!n) return "0 B";
  const u = ["B", "KiB", "MiB", "GiB", "TiB"];
  let i = 0;
  while (n >= 1024 && i < u.length - 1) {
    n /= 1024;
    ++i;
  }
  return `${n.toFixed(i ? 1 : 0)} ${u[i]}`;
}

/** A length in seconds: "4.20 s", "1:05.4", "1:02:03". */
export function fmtSeconds(s) {
  if (s == null || !Number.isFinite(s)) return "–";
  const sign = s < 0 ? "−" : "";
  s = Math.abs(s);
  if (s < 60) return `${sign}${s.toFixed(s < 10 ? 2 : 1)} s`;
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  if (h) return `${sign}${h}:${String(m).padStart(2, "0")}:${String(Math.floor(sec)).padStart(2, "0")}`;
  return `${sign}${m}:${sec.toFixed(1).padStart(4, "0")}`;
}

/** The first 8 characters of an id; the whole id goes into a title. */
export const shortId = (id) => (id ? String(id).slice(0, 8) : "–");

export const clone = (v) => JSON.parse(JSON.stringify(v));

/** JSON with sorted keys: the API answers its objects key-sorted, the UI builds them in any order. */
export function canon(value) {
  return JSON.stringify(value, (key, v) =>
    v && typeof v === "object" && !Array.isArray(v) ? Object.fromEntries(Object.keys(v).sort().map((k) => [k, v[k]])) : v,
  );
}

/** Copies text; falls back to a hidden textarea on plain http, where navigator.clipboard is missing. */
export async function copyText(text) {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch {
    const area = document.createElement("textarea");
    area.value = text;
    area.style.position = "fixed";
    area.style.opacity = "0";
    document.body.appendChild(area);
    area.select();
    const ok = document.execCommand("copy");
    area.remove();
    return ok;
  }
}

export function download(name, text, type = "application/json") {
  const url = URL.createObjectURL(new Blob([text], { type }));
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

/** Frame rate of a format name ("1080i29.97" → 29.97 frames per second). */
export function formatFps(name) {
  const m = /^\d+[pi]([\d.]+)$/.exec(name || "");
  return m ? Number(m[1]) : 50;
}

/** Peak (linear, 0..1) to dBFS. */
export const peakDb = (v) => (v > 0 ? 20 * Math.log10(v) : -120);

// ---- value lists -----------------------------------------------------------

/** Video patterns (§5.1) in groups; PLUGE applies to the bars. */
export const PATTERN_GROUPS = [
  {
    label: "Bars",
    items: [
      { value: "smpte_rp219", label: "SMPTE RP 219" },
      { value: "ebu_100_75", label: "EBU 100/75" },
      { value: "ebu_100_100", label: "EBU 100/100" },
      { value: "bars_75", label: "75 % bars" },
    ],
  },
  {
    label: "Greyscale",
    items: [
      { value: "grey_ramp_h", label: "Ramp horizontal" },
      { value: "grey_ramp_v", label: "Ramp vertical" },
      { value: "luma_steps", label: "Luma steps" },
    ],
  },
  {
    label: "Detail and geometry",
    items: [
      { value: "multiburst", label: "Multiburst" },
      { value: "sweep", label: "Sweep" },
      { value: "zone_plate", label: "Zone plate" },
      { value: "zone_plate_moving", label: "Zone plate moving" },
      { value: "checkerboard", label: "Checkerboard" },
      { value: "grid", label: "Grid" },
      { value: "circle", label: "Circle" },
      { value: "safe_area", label: "Safe area" },
    ],
  },
  {
    label: "Solid colours",
    items: [
      { value: "black", label: "Black" },
      { value: "white", label: "White" },
      { value: "red", label: "Red" },
      { value: "green", label: "Green" },
      { value: "blue", label: "Blue" },
      { value: "grey50", label: "Grey 50 %" },
    ],
  },
  {
    label: "Motion and timing",
    items: [
      { value: "motion", label: "Motion" },
      { value: "field_order", label: "Field order" },
      { value: "av_sync", label: "A/V sync" },
    ],
  },
];
export const PATTERNS = PATTERN_GROUPS.flatMap((g) => g.items);
export const PLUGE_PATTERNS = ["smpte_rp219", "ebu_100_75", "ebu_100_100", "bars_75"];
export const patternLabel = (v) => PATTERNS.find((p) => p.value === v)?.label || v || "–";

/** Audio signals (§5.2); `freq`: the frequency applies. */
export const SIGNALS = [
  { value: "sine", label: "Sine", freq: true },
  { value: "ident_freq", label: "Ident: tone per channel", title: "400 Hz + 100 Hz per channel" },
  { value: "ident_ebu", label: "EBU stereo ident", freq: true, title: "left channel interrupted" },
  { value: "ident_beeps", label: "Ident: beep count", title: "n beeps on channel n" },
  { value: "pink", label: "Pink noise" },
  { value: "white", label: "White noise" },
  { value: "sweep", label: "Sweep", title: "20 Hz – 20 kHz log sweep over 8 s" },
  { value: "polarity", label: "Polarity", title: "asymmetric pulse every 20 ms" },
  { value: "sync_beep", label: "Sync beep", title: "beep once per second, silence between" },
  { value: "silence", label: "Silence" },
];
export const signalLabel = (v) => SIGNALS.find((s) => s.value === v)?.label || v || "–";

/** Output formats parse_format accepts (§4): interlaced only 1080i25 and 1080i29.97. */
const RATES = ["23.98", "24", "25", "29.97", "30", "50", "59.94", "60"];
export const FORMATS = [
  ...RATES.map((r) => `1080p${r}`),
  "1080i25",
  "1080i29.97",
  ...RATES.map((r) => `720p${r}`),
  ...RATES.map((r) => `2160p${r}`),
];

export const KEY_MODES = [
  { value: "off", label: "Off", title: "fill only" },
  { value: "fill_key", label: "Fill + key", title: "two video/v210 flows" },
  { value: "v210a", label: "v210a", title: "one video/v210a flow with the key embedded" },
];
export const IDLE_KEYS = [
  { value: "opaque", label: "Opaque" },
  { value: "transparent", label: "Transparent" },
];
export const TC_SOURCES = [
  { value: "tai", label: "TAI", title: "time of day from TAI" },
  { value: "utc", label: "UTC" },
  { value: "local", label: "Local" },
  { value: "media", label: "Media", title: "timecode embedded in the item, else 00:00:00:00 at its start" },
  { value: "free", label: "Free run", title: "counts from free_start_frame in the configuration file" },
];
export const ATC_KINDS = [
  { value: "ltc", label: "ATC_LTC" },
  { value: "vitc1", label: "ATC_VITC1" },
  { value: "vitc2", label: "ATC_VITC2" },
];
export const PAUSE_AUDIO = [
  { value: "silence", label: "Silence" },
  { value: "hold_tone", label: "Hold tone" },
];
export const FONTS = ["DejaVu Sans", "DejaVu Sans Bold", "DejaVu Sans Mono"];
export const ANCHORS = ["tl", "tc", "tr", "ml", "mc", "mr", "bl", "bc", "br"];
export const PATHS = [
  { value: "bounce", label: "Bounce" },
  { value: "horizontal", label: "Horizontal" },
  { value: "vertical", label: "Vertical" },
  { value: "circle", label: "Circle" },
  { value: "diagonal", label: "Diagonal" },
];
/** Text placeholders, expanded every frame (§5.5). */
export const PLACEHOLDERS = ["{label}", "{timecode}", "{tai}", "{utc}", "{local}", "{frame}", "{item}", "{loop}", "{host}", "{flow}"];
/** Upload and re-convert options (§6.2). */
export const FITS = [
  { value: "fit", label: "Fit", title: "pillar- or letterbox" },
  { value: "fill", label: "Fill", title: "crop to fill the raster" },
  { value: "center", label: "1:1 centred" },
];
export const FPS_MODES = [
  { value: "drop", label: "Repeat / drop" },
  { value: "motion", label: "Motion interpolation", title: "FFmpeg minterpolate: slow" },
];
export const ALPHA_MODES = [
  { value: "straight", label: "Straight" },
  { value: "premultiplied", label: "Premultiplied" },
];

/** New text layer and moving box, with every field the API knows. */
export function newTextLayer() {
  return {
    text: "LIVE {timecode}",
    anchor: "mc",
    offset_x: 0,
    offset_y: 0,
    size: 6,
    font: "DejaVu Sans Bold",
    color: "#FFFFFF",
    opacity: 1,
    box: true,
    box_color: "#000000",
    box_opacity: 0.5,
    box_padding: 0.4,
    outline: false,
    outline_color: "#000000",
    shadow: false,
  };
}
export function newBox() {
  return { enabled: true, content: "builtin", path: "bounce", speed: 0.15, size: 0.12, opacity: 1, color: "#FFCC00", include_in_key: false, start_x: 0, start_y: 0 };
}
