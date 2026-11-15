//! The launcher's logic, without UI: the game's config file, the settings the launcher edits, the game's command
//! line, the crosshair's geometry and CS2's crosshair codes, and the launcher's own settings. launcher/macos's
//! LauncherCore is the same logic in Swift; `cargo test` checks this one.

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

pub const GAME_COUNT: usize = 3;
pub const GAME_TITLES: [&str; GAME_COUNT] = ["Allied Assault", "Spearhead", "Breakthrough"];
/// Each game's data folder, which is also its folder for configs
pub const GAME_DIRS: [&str; GAME_COUNT] = ["main", "mainta", "maintt"];

/// Lowercase cvar name -> value
pub type Cvars = BTreeMap<String, String>;

/// The values a config's `seta`/`set` lines leave, as the game reads them: quotes, `;` and `//` as the game's own
/// command parser handles them. Names are lowercased (cvar names don't depend on case).
pub fn parse_config(text: &str) -> Cvars {
    fn take(tokens: &mut Vec<String>, out: &mut Cvars) {
        if tokens.len() >= 3 {
            let cmd = tokens[0].to_ascii_lowercase();
            if matches!(cmd.as_str(), "set" | "seta" | "sets" | "setu") {
                out.insert(tokens[1].to_lowercase(), tokens[2..].join(" "));
            }
        }
        tokens.clear();
    }

    let b = text.as_bytes();
    let n = b.len();
    let at = |k: usize| if k < n { b[k] } else { 0 };
    let mut out = Cvars::new();
    let mut tokens = Vec::new();
    let mut star_comment = false;
    let mut i = 0;
    while i < n {
        let c = b[i];
        if star_comment {
            if c == b'*' && at(i + 1) == b'/' {
                star_comment = false;
                i += 2;
            } else {
                i += 1;
            }
            continue;
        }
        if c == b'\n' || c == b'\r' || c == b';' {
            // the end of a command (the game's command buffer splits there outside quotes)
            take(&mut tokens, &mut out);
            i += 1;
            continue;
        }
        if c <= b' ' {
            i += 1;
            continue;
        }
        if c == b'/' && at(i + 1) == b'/' {
            while i < n && b[i] != b'\n' && b[i] != b'\r' {
                i += 1;
            }
            continue;
        }
        if c == b'/' && at(i + 1) == b'*' {
            star_comment = true;
            i += 2;
            continue;
        }
        let start;
        let end;
        if c == b'"' {
            // a quoted token runs to the closing quote or the end of the line
            start = i + 1;
            i = start;
            while i < n && b[i] != b'"' && b[i] != b'\n' && b[i] != b'\r' {
                i += 1;
            }
            end = i;
            if i < n && b[i] == b'"' {
                i += 1;
            }
        } else {
            start = i;
            while i < n
                && b[i] > b' '
                && b[i] != b'"'
                && b[i] != b';'
                && !(b[i] == b'/' && (at(i + 1) == b'/' || at(i + 1) == b'*'))
            {
                i += 1;
            }
            end = i;
        }
        tokens.push(String::from_utf8_lossy(&b[start..end]).into_owned());
    }
    take(&mut tokens, &mut out);
    out
}

pub fn read_config(path: &Path) -> Option<Cvars> {
    let bytes = std::fs::read(path).ok()?;
    Some(parse_config(&String::from_utf8_lossy(&bytes)))
}

/// The first config that exists: for the game's own data folder, then for Allied Assault's (an expansion's first
/// start inherits it), in each home folder in turn, `omconfig.cfg` before the original game's `unnamedsoldier.cfg`.
pub fn find_config(homes: &[PathBuf], game: usize) -> Option<PathBuf> {
    let mut dirs = vec![GAME_DIRS[game]];
    if game != 0 {
        dirs.push(GAME_DIRS[0]);
    }
    for name in ["omconfig.cfg", "unnamedsoldier.cfg"] {
        for dir in &dirs {
            for home in homes {
                let path = home.join(dir).join("configs").join(name);
                if path.is_file() {
                    return Some(path);
                }
            }
        }
    }
    None
}

/// The settings the launcher edits. Each holds the value the game's config had when it was last read and the one
/// shown in the launcher; only the ones the player changed in the launcher go to the game, so a change made in the
/// game (its menus, the console) is never undone by starting it from here.
#[derive(Clone, Debug)]
pub struct CvarEdits {
    entries: Vec<Entry>,
}

#[derive(Clone, Debug)]
struct Entry {
    /// lowercase, to find it (cvar names don't depend on case)
    key: String,
    /// as written, to pass it
    name: String,
    def: String,
    value: String,
    loaded: String,
}

impl CvarEdits {
    pub fn new(defaults: &[(&str, &str)]) -> Self {
        let entries = defaults
            .iter()
            .map(|(name, def)| Entry {
                key: name.to_lowercase(),
                name: name.to_string(),
                def: def.to_string(),
                value: def.to_string(),
                loaded: def.to_string(),
            })
            .collect();
        CvarEdits { entries }
    }

    fn find(&self, name: &str) -> Option<&Entry> {
        let key = name.to_lowercase();
        self.entries.iter().find(|e| e.key == key)
    }

    pub fn get(&self, name: &str) -> &str {
        self.find(name).map_or("", |e| e.value.as_str())
    }

    pub fn set(&mut self, name: &str, value: &str) {
        let key = name.to_lowercase();
        if let Some(e) = self.entries.iter_mut().find(|e| e.key == key) {
            e.value = value.to_string();
        }
    }

    #[cfg(test)]
    pub fn changed(&self, name: &str) -> bool {
        self.find(name).is_some_and(|e| e.value != e.loaded)
    }

    /// Takes the config's values (the defaults for the ones it doesn't have); a value changed in the launcher stays
    pub fn load(&mut self, config: &Cvars) {
        for e in &mut self.entries {
            let changed = e.value != e.loaded;
            e.loaded = config.get(&e.key).cloned().unwrap_or_else(|| e.def.clone());
            if !changed {
                e.value = e.loaded.clone();
            }
        }
    }

    /// The changed values, in the order of the defaults, by their names as written
    pub fn changes(&self) -> Vec<(String, String)> {
        self.entries.iter().filter(|e| e.value != e.loaded).map(|e| (e.name.clone(), e.value.clone())).collect()
    }

    /// The game was started with the changes: they are the config's values now
    pub fn applied(&mut self) {
        for e in &mut self.entries {
            e.loaded = e.value.clone();
        }
    }
}

/// One value on the game's command line. The game joins its arguments into one line, quoting the ones with a space,
/// and splits that line into commands at each `+` and `;` outside quotes: a value is cleaned of quotes and control
/// characters, and quoted when it has none of its own (no space) but could be split or cut (`+`, `;`, `//`).
pub fn engine_arg(value: &str) -> String {
    let v: String = value
        .chars()
        .filter_map(|c| match c {
            '\t' => Some(' '),
            '"' => None,
            c if (c as u32) < 0x20 || c as u32 == 0x7f => None,
            c => Some(c),
        })
        .collect();
    if v.contains(' ') {
        return v; // the game puts it in quotes itself
    }
    if v.is_empty() || v.contains('+') || v.contains(';') || v.contains("//") || v.contains("/*") {
        return format!("\"{v}\"");
    }
    v
}

#[derive(Clone, Debug, Default)]
pub struct Launch {
    /// fs_homepath: where the game writes everything (the game folder), if not empty
    pub homepath: String,
    /// fs_basepath: the game folder when the game isn't in it, else empty
    pub basepath: String,
    pub game: usize,
    pub cvars: Vec<(String, String)>,
    pub address: String,
    pub password: String,
    pub rcon: String,
}

/// The game's arguments: +set fs_homepath, +set fs_basepath, +set com_target_game N, the cvars, the passwords,
/// +connect
pub fn build_args(launch: &Launch) -> Vec<String> {
    let mut args: Vec<String> = Vec::new();
    let mut set = |name: &str, value: &str| {
        args.extend(["+set".to_string(), name.to_string(), engine_arg(value)]);
    };
    if !launch.homepath.is_empty() {
        set("fs_homepath", &launch.homepath);
    }
    if !launch.basepath.is_empty() {
        set("fs_basepath", &launch.basepath);
    }
    set("com_target_game", &launch.game.to_string());
    for (name, value) in &launch.cvars {
        set(name, value);
    }
    if !launch.password.is_empty() {
        set("password", &launch.password);
    }
    if !launch.rcon.is_empty() {
        set("rconPassword", &launch.rcon);
    }
    if !launch.address.is_empty() {
        args.push("+connect".into());
        args.push(engine_arg(&launch.address));
    }
    args
}

pub fn same_address(a: &str, b: &str) -> bool {
    fn key(s: &str) -> String {
        let k = s.trim().to_lowercase();
        if k.contains(':') {
            k
        } else {
            k + ":12203" // the game's default port
        }
    }
    key(a) == key(b)
}

/// "FFFFFF", "#ffffff" -> 0xFFFFFF; None if it isn't 6 hex digits (the game then draws white)
pub fn parse_color(text: &str) -> Option<u32> {
    let hex = text.trim();
    let hex = hex.strip_prefix('#').unwrap_or(hex);
    if hex.len() != 6 || !hex.chars().all(|c| c.is_ascii_hexdigit()) {
        return None;
    }
    u32::from_str_radix(hex, 16).ok()
}

pub fn color_hex(rgb: u32) -> String {
    format!("{:06X}", rgb & 0xFF_FFFF)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CrossBox {
    pub x: i32,
    pub y: i32,
    pub w: i32,
    pub h: i32,
}

/// The crosshair's settings that shape it: sizes in pixels at 1080 lines, the outline 0 (none), 1 (full) or 2 (half)
pub struct Crosshair {
    pub length: f64,
    pub gap: f64,
    pub thickness: f64,
    pub outline: i32,
    pub dot: bool,
    pub t: bool,
}

/// The crosshair as cg_drawtools.cpp's CG_UpdateCrosshair lays it out at 1080 lines (CS2's static cross), in HUD
/// pixels from the screen's middle pixel
pub struct CrossLayout {
    /// the arms and the dot, joined into boxes that don't overlap
    pub boxes: Vec<CrossBox>,
    /// the outline, drawn first, joined the same way
    pub outline: Vec<CrossBox>,
    /// the square around the crosshair's middle that holds it all: its width (and height), and its top left corner
    /// (the same both ways)
    pub extent: i32,
    pub start: i32,
}

pub fn crosshair_boxes(c: &Crosshair) -> CrossLayout {
    // in whole pixels as the game computes them, at 1080 lines: half away from zero, at least 1 above 0, else 0
    fn pixels(value: f64) -> i32 {
        let v = value as f32;
        if !(v > 0.0) {
            return 0; // NaN too
        }
        ((v.min(4096.0) * 1080.0 / 1080.0 + 0.5) as i32).max(1)
    }
    let b = |x, y, w, h| CrossBox { x, y, w, h };
    let (l, t, g) = (pixels(c.length), pixels(c.thickness), pixels(c.gap));
    // a line t thick has (t + 1) / 2 pixels before the middle one; the gap counts from it, a pixel less after it
    // when t is odd
    let (before, odd) = ((t + 1) / 2, t & 1);
    let mut pieces = Vec::new();
    if t > 0 && l > 0 {
        pieces.extend([b(-g - l, -before, l, t), b(g - odd, -before, l, t), b(-before, g - odd, t, l)]);
        if !c.t {
            pieces.push(b(-before, -g - l, t, l));
        }
    }
    if t > 0 && c.dot {
        pieces.push(b(-before, -before, t, t));
    }
    // a pixel above (over the corners) and left of every piece; full: below and right too
    let mode = c.outline.clamp(0, 2);
    let mut strips = Vec::new();
    for p in pieces.iter().filter(|_| mode != 0) {
        strips.extend([b(p.x - 1, p.y - 1, p.w + if mode == 1 { 2 } else { 1 }, 1), b(p.x - 1, p.y, 1, p.h)]);
        if mode == 1 {
            strips.extend([b(p.x - 1, p.y + p.h, p.w + 2, 1), b(p.x + p.w, p.y, 1, p.h)]);
        }
    }
    // the middle is -odd / 2 both ways: the farthest edge from it, twice, is the square's width
    let edges = pieces.iter().chain(&strips).flat_map(|r| [r.x, r.x + r.w, r.y, r.y + r.h]);
    let extent = edges.map(|e| (2 * e + odd).abs()).max().unwrap_or(0);
    let (boxes, outline) = (crosshair_union(&pieces), crosshair_union(&strips));
    CrossLayout { boxes, outline, extent, start: -(extent + odd) / 2 }
}

/// The pixels of boxes as boxes that don't overlap, as CG_CrosshairUnion joins them (at most 64), so that a see-through
/// crosshair blends each pixel once: the bands between the boxes' top and bottom edges, the boxes across each band
/// joined, a box going on down into the next band when it has the same span there
fn crosshair_union(boxes: &[CrossBox]) -> Vec<CrossBox> {
    let mut edges: Vec<i32> = boxes.iter().flat_map(|r| [r.y, r.y + r.h]).collect();
    edges.sort();
    let mut out: Vec<CrossBox> = Vec::new();
    for band in edges.windows(2).filter(|e| e[0] != e[1]) {
        let (y0, y1, above) = (band[0], band[1], out.len());
        let mut spans: Vec<(i32, i32)> =
            boxes.iter().filter(|r| r.y <= y0 && r.y + r.h >= y1 && r.w > 0).map(|r| (r.x, r.x + r.w)).collect();
        spans.sort_by_key(|s| s.0);
        let mut i = 0;
        while i < spans.len() {
            let (x0, mut x1) = spans[i];
            i += 1;
            while i < spans.len() && spans[i].0 <= x1 {
                x1 = x1.max(spans[i].1);
                i += 1;
            }
            if let Some(r) = out[..above].iter_mut().find(|r| r.x == x0 && r.w == x1 - x0 && r.y + r.h == y0) {
                r.h += y1 - y0;
            } else if out.len() < 64 {
                out.push(CrossBox { x: x0, y: y0, w: x1 - x0, h: y1 - y0 });
            }
        }
    }
    out
}

/// The characters of CS2's crosshair codes, a digit of base 57 each
const CODE_DIGITS: &[u8; 57] = b"ABCDEFGHJKLMNOPQRSTUVWXYZabcdefhijkmnopqrstuvwxyz23456789";

/// The settings a CS2 crosshair code sets, as the game's crosshair_import sets them (code/client/cl_crosshair.c,
/// which describes the format): the text's first word, else why it isn't a code CS2 writes now
pub fn crosshair_import(text: &str) -> Result<Vec<(&'static str, String)>, String> {
    let code = text.split([' ', '\t', '\r', '\n']).find(|w| !w.is_empty()).unwrap_or("");
    if code.is_empty() {
        return Err("no code: copy the crosshair's code in CS2 first".into());
    }
    if code.starts_with("CSGO") {
        let why = "an old CSGO- code: CS2 no longer takes those either, copy the crosshair's new code from CS2";
        return Err(why.into());
    }
    if !code.starts_with("CS") || code.len() != 46 {
        return Err("not a crosshair code (CS and 44 letters and digits)".into());
    }
    // a number in base 57, its last character the most significant digit, into 32 bytes, byte 0 the most significant
    let mut b = [0u8; 32];
    for c in code.bytes().skip(2).rev() {
        let Some(digit) = CODE_DIGITS.iter().position(|&d| d == c) else {
            return Err("not a crosshair code (a character CS2 never writes: a typo?)".into());
        };
        let mut carry = digit as u32;
        for byte in b.iter_mut().rev() {
            carry += *byte as u32 * 57;
            *byte = carry as u8;
            carry >>= 8;
        }
        if carry != 0 {
            return Err("not a crosshair code (a typo?)".into());
        }
    }
    if b[0] != b[1..].iter().fold(0u8, |sum, &x| sum.wrapping_add(x)) {
        return Err("the code is garbled (its checksum doesn't match: a typo?)".into());
    }
    if b[1] != 1 {
        return Err(format!("a crosshair code of a newer kind (version {}) than this game reads", b[1]));
    }
    let height = u16::from_le_bytes([b[2], b[3]]);
    if height == 0 {
        return Err("the code has no screen height".into());
    }

    // sizes are pixels at the code's height (240 at least): at 1080 lines, in the game's float and printf's "%g"
    // (they stay within 0.01 to 150000: never its exponent form)
    let scale = 1080.0f32 / height.max(240) as f32;
    let size = |pixels: f32| {
        let v = (pixels * scale) as f64;
        let exponent: i32 = format!("{v:.5e}").rsplit('e').next().and_then(|e| e.parse().ok()).unwrap_or(0);
        let fixed = format!("{:.*}", (5 - exponent).max(0) as usize, v);
        if fixed.contains('.') { fixed.trim_end_matches('0').trim_end_matches('.').to_string() } else { fixed }
    };
    let dot_only = b[4] & 31 == 6;
    let hex = |rgb: &[u8]| format!("{:02X}{:02X}{:02X}", rgb[0], rgb[1], rgb[2]);
    Ok(vec![
        ("cg_crosshair_length", if dot_only { "0".into() } else { size(b[16] as f32) }),
        ("cg_crosshair_gap", size(i16::from_le_bytes([b[14], b[15]]) as f32)),
        ("cg_crosshair_thickness", size((b[13] & 63) as f32)),
        ("cg_crosshair_color", hex(&b[5..8])),
        ("cg_crosshair_alpha", b[8].to_string()),
        ("cg_crosshair_outline", (b[13] >> 6).to_string()),
        ("cg_crosshair_outline_color", hex(&b[9..12])),
        ("cg_crosshair_outline_alpha", b[12].to_string()),
        ("cg_crosshair_dot", ((dot_only || b[4] & 64 != 0) as i32).to_string()),
        ("cg_crosshair_t", (b[4] >> 7).to_string()),
        ("cg_crosshair_code", code.to_string()),
    ])
}

/// The game's window modes on the Vulkan port, as r_fullscreen and r_noborder pick them
pub const MODE_TITLES: [&str; 3] = ["Fullscreen", "Borderless window", "Window"];
pub const MODE_FULLSCREEN: usize = 0;
pub const MODE_BORDERLESS: usize = 1;
pub const MODE_WINDOW: usize = 2;

fn int(s: &str) -> i64 {
    s.trim().parse::<f64>().map_or(0, |v| v as i64)
}

pub fn mode_from_cvars(fullscreen: &str, noborder: &str) -> usize {
    if int(fullscreen) != 0 {
        MODE_FULLSCREEN
    } else if int(noborder) != 0 {
        MODE_BORDERLESS
    } else {
        MODE_WINDOW
    }
}

pub fn mode_to_cvars(mode: usize, edits: &mut CvarEdits) {
    match mode {
        MODE_FULLSCREEN => {
            // r_noborder only counts for a window
            if int(edits.get("r_fullscreen")) == 0 {
                edits.set("r_fullscreen", "1");
            }
        }
        MODE_BORDERLESS => {
            edits.set("r_fullscreen", "0");
            edits.set("r_noborder", "1");
        }
        _ => {
            edits.set("r_fullscreen", "0");
            edits.set("r_noborder", "0");
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Resolution {
    pub width: i32,
    pub height: i32,
}

/// The game's numbered modes, r_mode 0 to 11 (the renderers' r_vidModes): a config can hold one
pub const R_MODES: [(i32, i32); 12] = [
    (320, 240), (400, 300), (512, 384), (640, 480), (800, 600), (960, 720), (1024, 768), (1152, 864), (1280, 1024),
    (1600, 1200), (2048, 1536), (856, 480),
];

/// A size's aspect ratio, by the common name it's within 3% of (1366 x 768 is 16:9), else as a reduced fraction
pub fn aspect_ratio(width: i32, height: i32) -> String {
    const NAMED: [(i32, i32); 7] = [(4, 3), (5, 4), (3, 2), (16, 10), (16, 9), (21, 9), (32, 9)];
    if width <= 0 || height <= 0 {
        return String::new();
    }
    let ratio = width as f64 / height as f64;
    if let Some((w, h)) = NAMED.iter().find(|&&(w, h)| (ratio / (w as f64 / h as f64) - 1.0).abs() < 0.03) {
        return format!("{w}:{h}");
    }
    let (mut a, mut b) = (width, height);
    while b != 0 {
        (a, b) = (b, a % b);
    }
    format!("{}:{}", width / a, height / a)
}

/// The sizes to offer: the display's, and the common ones and numbered modes (r_mode 0 to 11) that fit it, from
/// 640 x 480 up, largest first, without duplicates (a config's own size, if it isn't one of them, is added by
/// resolution_entries)
pub fn resolution_list(display_modes: &[Resolution], desktop: Option<Resolution>) -> Vec<Resolution> {
    const COMMON: [(i32, i32); 18] = [
        (3840, 2160), (3440, 1440), (2560, 1600), (2560, 1440), (2560, 1080), (1920, 1200), (1920, 1080),
        (1680, 1050), (1600, 900), (1440, 900), (1366, 768), (1280, 1024), (1280, 960), (1280, 800),
        (1280, 720), (1024, 768), (800, 600), (640, 480),
    ];
    let mut list: Vec<Resolution> = Vec::new();
    let mut add = |r: Resolution| {
        if r.width >= MIN_RESOLUTION.0 && r.height >= MIN_RESOLUTION.1 && !list.contains(&r) {
            list.push(r);
        }
    };
    if let Some(d) = desktop {
        add(d);
    }
    for &r in display_modes {
        add(r);
    }
    for (w, h) in COMMON.into_iter().chain(R_MODES) {
        if desktop.is_none_or(|d| w <= d.width && h <= d.height) {
            add(Resolution { width: w, height: h });
        }
    }
    list.sort_by(|a, b| b.width.cmp(&a.width).then(b.height.cmp(&a.height)));
    list
}

/// The smallest size the launcher offers or passes
pub const MIN_RESOLUTION: (i32, i32) = (640, 480);

/// A config's size below 640 x 480 (r_mode 0 to 2, or a custom size) as 640 x 480: the menu shows it, and starting
/// the game passes it, as a change made in the launcher
pub fn raise_small_resolution(edits: &mut CvarEdits) {
    let mode = int(edits.get("r_mode"));
    let size = if mode == -1 {
        Some((int(edits.get("r_customwidth")) as i32, int(edits.get("r_customheight")) as i32))
    } else {
        usize::try_from(mode).ok().and_then(|m| R_MODES.get(m)).copied()
    };
    if let Some((w, h)) = size {
        if w < MIN_RESOLUTION.0 || h < MIN_RESOLUTION.1 {
            edits.set("r_mode", "-1");
            edits.set("r_customwidth", &MIN_RESOLUTION.0.to_string());
            edits.set("r_customheight", &MIN_RESOLUTION.1.to_string());
        }
    }
}

/// An entry of the resolution menu: a size (r_mode -1 with r_customwidth and r_customheight)
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ResolutionEntry {
    pub mode: i64,
    pub width: i32,
    pub height: i32,
}

impl ResolutionEntry {
    pub fn label(&self) -> String {
        format!("{} \u{00D7} {} ({})", self.width, self.height, aspect_ratio(self.width, self.height))
    }
}

/// The menu's entries for the sizes offered and the config's own (r_mode, r_customwidth, r_customheight), and the
/// index of the config's. A numbered mode (r_mode 0 to 11) is its size, and the desktop's resolution (r_mode -2, the
/// game's default, or a number the game doesn't have) the desktop's size; nothing is passed to the game unless the
/// player picks another entry, so r_mode -2 keeps following the desktop.
pub fn resolution_entries(
    sizes: &[Resolution],
    desktop: Option<Resolution>,
    edits: &CvarEdits,
) -> (Vec<ResolutionEntry>, usize) {
    let entry = |width, height| ResolutionEntry { mode: -1, width, height };
    let mut entries: Vec<ResolutionEntry> = sizes.iter().map(|r| entry(r.width, r.height)).collect();
    let mode = int(edits.get("r_mode"));
    let current = if mode == -1 {
        entry(int(edits.get("r_customwidth")) as i32, int(edits.get("r_customheight")) as i32)
    } else if let Some(&(width, height)) = usize::try_from(mode).ok().and_then(|m| R_MODES.get(m)) {
        entry(width, height)
    } else if let Some(d) = desktop {
        entry(d.width, d.height)
    } else if let Some(&first) = entries.first() {
        first
    } else {
        entry(MIN_RESOLUTION.0, MIN_RESOLUTION.1)
    };
    if let Some(k) = entries.iter().position(|e| *e == current) {
        return (entries, k);
    }
    // the config's own size, where it belongs in the list
    let k = entries
        .iter()
        .position(|e| e.width < current.width || (e.width == current.width && e.height < current.height))
        .unwrap_or(entries.len());
    entries.insert(k, current);
    (entries, k)
}

/// The launcher's own settings, as the game folder's mohlauncher.json holds them (the macOS launcher's are the same)
#[derive(Clone, Debug, Default, Serialize, Deserialize)]
#[serde(default, rename_all = "camelCase")]
pub struct Settings {
    pub game: usize,
    pub slots: Vec<GameSlot>,
    /// Quit the launcher once the game has started (Single player or Connect)
    pub close_on_start: bool,
}

/// Per game: the server fields and the servers played on
#[derive(Clone, Debug, Default, Serialize, Deserialize)]
#[serde(default, rename_all = "camelCase")]
pub struct GameSlot {
    pub address: String,
    pub password: String,
    pub rcon: String,
    pub servers: Vec<Server>,
}

/// A server in the list: what it's called there (its address until it's renamed), its address and the passwords
/// used on it. The player's own name isn't part of it: that's the game's.
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(default, rename_all = "camelCase")]
pub struct Server {
    pub label: String,
    pub address: String,
    pub password: String,
    pub rcon: String,
}

impl Settings {
    pub fn from_json(text: &str) -> Settings {
        let mut s: Settings = serde_json::from_str(text).unwrap_or_default();
        s.normalize();
        s
    }

    fn normalize(&mut self) {
        if self.game >= GAME_COUNT {
            self.game = 0;
        }
        self.slots.resize_with(GAME_COUNT, GameSlot::default);
        for slot in &mut self.slots {
            slot.servers.retain(|s| !s.address.trim().is_empty());
            for s in &mut slot.servers {
                if s.label.trim().is_empty() {
                    s.label = s.address.clone();
                }
            }
        }
    }

    /// Keeps the server in the game's list (added at the end), with the passwords used
    pub fn remember_server(&mut self, game: usize, address: &str, password: &str, rcon: &str) {
        let servers = &mut self.slots[game].servers;
        if let Some(s) = servers.iter_mut().find(|s| same_address(&s.address, address)) {
            s.address = address.to_string();
            s.password = password.to_string();
            s.rcon = rcon.to_string();
        } else {
            servers.push(Server {
                label: address.to_string(),
                address: address.to_string(),
                password: password.to_string(),
                rcon: rcon.to_string(),
            });
        }
    }
}

/// The servers the launcher comes with, first in their game's list (game, name, address): they can't be renamed or
/// removed
pub const BUILTIN_SERVERS: [(usize, &str, &str); 1] = [(0, "Forte FFA", "151.80.234.90:12203")];

/// A server as the list shows it
#[derive(Clone, Debug, PartialEq)]
pub struct Listed {
    /// with the passwords last used on it
    pub server: Server,
    /// its place in the game's saved servers, if it's there
    pub saved: Option<usize>,
    pub builtin: bool,
}

/// The game's list: its built-in servers, then the ones the player joined (a built-in one's entry, which keeps the
/// passwords used, isn't listed twice)
pub fn listed_servers(settings: &Settings, game: usize) -> Vec<Listed> {
    let saved = &settings.slots[game].servers;
    let mut list = Vec::new();
    for &(_, name, address) in BUILTIN_SERVERS.iter().filter(|b| b.0 == game) {
        let k = saved.iter().position(|s| same_address(&s.address, address));
        let (password, rcon) = k.map_or_else(Default::default, |k| (saved[k].password.clone(), saved[k].rcon.clone()));
        let server = Server { label: name.into(), address: address.into(), password, rcon };
        list.push(Listed { server, saved: k, builtin: true });
    }
    for (k, s) in saved.iter().enumerate() {
        if !BUILTIN_SERVERS.iter().any(|b| b.0 == game && same_address(b.2, &s.address)) {
            list.push(Listed { server: s.clone(), saved: Some(k), builtin: false });
        }
    }
    list
}

/// A path without the verbatim prefix Windows' canonical paths have (`\\?\C:\x`, `\\?\UNC\server\share\x`), for the
/// game's fs_homepath and fs_basepath: the game appends to the path it's given and merges repeated separators, which
/// turns `\\?\` into `\?\`, a folder it can't create. Any other path, and one that isn't Unicode, is kept as it is.
pub fn plain_path(path: &Path) -> PathBuf {
    let Some(text) = path.to_str() else { return path.to_path_buf() };
    if let Some(rest) = text.strip_prefix(r"\\?\UNC\") {
        return PathBuf::from(format!(r"\\{rest}"));
    }
    if let Some(rest) = text.strip_prefix(r"\\?\") {
        let b = rest.as_bytes();
        if b.len() >= 2 && b[0].is_ascii_alphabetic() && b[1] == b':' {
            return PathBuf::from(rest);
        }
    }
    path.to_path_buf()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeSet;

    #[test]
    fn parses_configs() {
        let text = "// generated by OpenMoHAA, do not modify\n\
            unbindall\n\
            bind TAB \"+scores\"\n\
            seta name \"Some Body\"\r\n\
            seta cg_crosshair_length \"5\"\n\
            SETA CG_Crosshair_Color \"#00ff00\" // green\n\
            set r_mode -1; seta r_customwidth \"2560\"; seta r_customheight 1440\n\
            seta password \"a;b//c\"\n\
            /* seta r_fullscreen \"0\" */ seta r_noborder \"1\"\n\
            seta empty \"\"\n\
            seta unterminated \"abc\n\
            seta spaced one two\n\
            seta \u{00FC}n\u{00EF}code \"\u{00E4}\"\n";
        let c = parse_config(text);
        assert_eq!(c["name"], "Some Body");
        assert_eq!(c["cg_crosshair_length"], "5");
        assert_eq!(c["cg_crosshair_color"], "#00ff00");
        assert_eq!(c["r_mode"], "-1");
        assert_eq!(c["r_customwidth"], "2560");
        assert_eq!(c["r_customheight"], "1440");
        assert_eq!(c["password"], "a;b//c");
        assert!(!c.contains_key("r_fullscreen"));
        assert_eq!(c["r_noborder"], "1");
        assert_eq!(c["empty"], "");
        assert_eq!(c["unterminated"], "abc");
        assert_eq!(c["spaced"], "one two");
        assert_eq!(c["\u{00FC}n\u{00EF}code"], "\u{00E4}");
        assert!(!c.contains_key("tab"));
    }

    #[test]
    fn keeps_the_launchers_changes_only() {
        let mut e = CvarEdits::new(&[("name", ""), ("cg_crosshair_length", "4"), ("r_fullscreen", "1")]);
        assert_eq!(e.get("cg_crosshair_length"), "4");
        assert!(e.changes().is_empty());

        e.load(&Cvars::from([("name".into(), "Player".into()), ("cg_crosshair_length".into(), "6".into())]));
        assert_eq!(e.get("name"), "Player");
        assert_eq!(e.get("cg_crosshair_length"), "6");
        assert_eq!(e.get("r_fullscreen"), "1");
        assert!(e.changes().is_empty());

        e.set("cg_crosshair_length", "8");
        assert!(e.changed("cg_crosshair_length"));
        // a change made in the game to another setting comes in; the launcher's own change stays
        e.load(&Cvars::from([("name".into(), "Renamed".into()), ("cg_crosshair_length".into(), "7".into())]));
        assert_eq!(e.get("name"), "Renamed");
        assert_eq!(e.get("cg_crosshair_length"), "8");
        assert_eq!(e.changes(), vec![("cg_crosshair_length".to_string(), "8".to_string())]);

        e.set("cg_crosshair_length", "7");
        assert!(e.changes().is_empty());

        e.set("r_fullscreen", "0");
        e.applied();
        assert!(e.changes().is_empty());
        e.load(&Cvars::from([("r_fullscreen".into(), "0".into())]));
        assert_eq!(e.get("r_fullscreen"), "0");
        assert!(!e.changed("r_fullscreen"));
    }

    #[test]
    fn builds_the_command_line() {
        assert_eq!(engine_arg("plain"), "plain");
        assert_eq!(engine_arg("with space"), "with space");
        assert_eq!(engine_arg("a;b"), "\"a;b\"");
        assert_eq!(engine_arg("a+b"), "\"a+b\"");
        assert_eq!(engine_arg("http://x"), "\"http://x\"");
        assert_eq!(engine_arg("say \"hi\""), "say hi");
        assert_eq!(engine_arg(""), "\"\"");
        assert_eq!(engine_arg("tab\there"), "tab here");
        assert_eq!(engine_arg("\u{00FC}n\u{00EF}"), "\u{00FC}n\u{00EF}");

        let launch = Launch {
            homepath: String::new(),
            basepath: String::new(),
            game: 1,
            cvars: vec![("name".into(), "A B".into()), ("cg_crosshair_color".into(), "00FF00".into())],
            address: "1.2.3.4:12203".into(),
            password: "pw;1".into(),
            rcon: "r".into(),
        };
        let want = [
            "+set", "com_target_game", "1", "+set", "name", "A B", "+set", "cg_crosshair_color", "00FF00", "+set",
            "password", "\"pw;1\"", "+set", "rconPassword", "r", "+connect", "1.2.3.4:12203",
        ];
        assert_eq!(build_args(&launch), want);
        assert_eq!(build_args(&Launch::default()), ["+set", "com_target_game", "0"]);
        let elsewhere = Launch { homepath: "/games/MOH AA".into(), basepath: "/games/MOH AA".into(), ..Default::default() };
        assert_eq!(
            build_args(&elsewhere),
            ["+set", "fs_homepath", "/games/MOH AA", "+set", "fs_basepath", "/games/MOH AA", "+set", "com_target_game", "0"]
        );
    }

    /// CG_UpdateCrosshair's layout (CS2's static cross) as the pixels it fills at 1920x1080, the middle one 960, 540
    #[test]
    fn crosshair() {
        let shape = |length, gap, thickness, outline, dot, t| Crosshair { length, gap, thickness, outline, dot, t };
        // the screen's pixels the boxes fill, each once
        let screen = |boxes: &[CrossBox]| {
            let mut filled = BTreeSet::new();
            for r in boxes {
                for (x, y) in (r.x..r.x + r.w).flat_map(|x| (r.y..r.y + r.h).map(move |y| (x, y))) {
                    assert!(filled.insert((960 + x, 540 + y)), "{x}, {y} twice");
                }
            }
            filled
        };
        // columns x0 to x1 by rows y0 to y1
        let pixels = |rects: &[(i32, i32, i32, i32)]| -> BTreeSet<(i32, i32)> {
            let rect = |&(x0, x1, y0, y1): &(i32, i32, i32, i32)| {
                (x0..=x1).flat_map(move |x| (y0..=y1).map(move |y| (x, y)))
            };
            rects.iter().flat_map(rect).collect()
        };

        // the default, as before: arms at x 948-955 and 964-971 (rows 539-540), y 528-535 and 544-551 (columns 959-960)
        let c = crosshair_boxes(&shape(8.0, 4.0, 2.0, 0, false, false));
        let arms = pixels(&[(948, 955, 539, 540), (964, 971, 539, 540), (959, 960, 528, 535), (959, 960, 544, 551)]);
        assert_eq!(screen(&c.boxes), arms);
        assert!(c.outline.is_empty());
        assert_eq!((c.extent, c.start), (24, -12));
        // full outline: a pixel round each piece, not scaled
        let c = crosshair_boxes(&shape(8.0, 4.0, 2.0, 1, false, false));
        let around = pixels(&[(947, 956, 538, 541), (963, 972, 538, 541), (958, 961, 527, 536), (958, 961, 543, 552)]);
        assert_eq!(screen(&c.outline), &around - &arms);
        assert_eq!((c.extent, c.start), (26, -13));

        // thickness 1: the line on the pixel before the middle, the gap a pixel less after it
        let c = crosshair_boxes(&shape(8.0, 4.0, 1.0, 0, false, false));
        let want = pixels(&[(948, 955, 539, 539), (963, 970, 539, 539), (959, 959, 528, 535), (959, 959, 543, 550)]);
        assert_eq!(screen(&c.boxes), want);
        assert_eq!((c.extent, c.start), (23, -12));

        // length 1, gap 2, thickness 8, half outline, as in a CS2 screenshot; the outline above and left of each piece
        let c = crosshair_boxes(&shape(1.0, 2.0, 8.0, 2, false, false));
        let want = pixels(&[(957, 957, 536, 543), (962, 962, 536, 543), (956, 963, 537, 537), (956, 963, 542, 542)]);
        assert_eq!(screen(&c.boxes), want);
        let strips = [(956, 957, 535, 535), (956, 956, 536, 543), (961, 962, 535, 535), (961, 961, 536, 543)];
        let strips = strips.into_iter().chain([(955, 963, 541, 541), (955, 955, 542, 542), (955, 963, 536, 536)]);
        assert_eq!(screen(&c.outline), pixels(&strips.chain([(955, 955, 537, 537)]).collect::<Vec<_>>()));

        // gap 0: the arms meet; the dot over them, each pixel once; T-style: no top arm
        let c = crosshair_boxes(&shape(5.0, 0.0, 2.0, 0, true, true));
        assert_eq!(screen(&c.boxes), pixels(&[(955, 964, 539, 540), (959, 960, 541, 544)]));
        // length 0: the dot alone, the middle t by t; thickness 0: nothing
        let c = crosshair_boxes(&shape(0.0, 4.0, 4.0, 0, true, false));
        assert_eq!(screen(&c.boxes), pixels(&[(958, 961, 538, 541)]));
        assert!(crosshair_boxes(&shape(8.0, 4.0, 0.0, 1, true, false)).boxes.is_empty());

        // half away from zero, at least 1 above 0, 4096 at most; a negative gap (or NaN) draws as 0; outline 0 to 2
        let c = crosshair_boxes(&shape(0.2, -3.0, 2.5, 7, false, false));
        assert_eq!(screen(&c.boxes), pixels(&[(959, 959, 538, 540), (958, 960, 539, 539)]));
        assert!(!c.outline.is_empty());
        let c = crosshair_boxes(&shape(5000.0, f64::NAN, 1.0, -1, false, false));
        assert_eq!((c.boxes[1], c.outline.len()), (CrossBox { x: -4096, y: -1, w: 8191, h: 1 }, 0));
    }

    /// The game's own test codes (code/client/tests/test_crosshair.c)
    #[test]
    fn crosshair_code() {
        let get = |text: &str| crosshair_import(text).map(|v| v.into_iter().collect::<BTreeMap<_, _>>());
        // red, dot, at 768 lines: sizes at 1080 lines as the game writes them
        let v = get("CSvbPubOq37zTGqtsPTP5QTrp5CB4xFXiKRLfzJsm49ZRe").unwrap();
        let want = [("length", "7.03125"), ("gap", "0"), ("thickness", "1.40625"), ("color", "FF0000")];
        for (name, value) in want.into_iter().chain([("alpha", "255"), ("outline", "0"), ("dot", "1"), ("t", "0")]) {
            assert_eq!(v[format!("cg_crosshair_{name}").as_str()], value, "{name}");
        }
        assert_eq!(v["cg_crosshair_code"], "CSvbPubOq37zTGqtsPTP5QTrp5CB4xFXiKRLfzJsm49ZRe");
        assert_eq!(v.len(), 11);
        // the first word, as copied with spaces or a line break around it
        let v = get(" CSHt3ObzKS3ikGJLRBVrKjPUrhOCikyke53na8cJFtNZKa\r\n").unwrap();
        assert_eq!(v["cg_crosshair_code"], "CSHt3ObzKS3ikGJLRBVrKjPUrhOCikyke53na8cJFtNZKa");
        // Dot Only: no arms, the dot
        let v = get("CSUyBmKsvnUffbDXZTmd9GxnFxzmKaO6VAvnEphKt6jQfJ").unwrap();
        assert_eq!((v["cg_crosshair_length"].as_str(), v["cg_crosshair_dot"].as_str()), ("0", "1"));
        // height 100 (240 then), gap -32768, thickness 63, outline 3
        let v = get("CS7uw3wMkJBJ4rMDLEaYbyfTJFFPvkLAhOmBFi8zOM6H2d").unwrap();
        let got = ["gap", "thickness", "outline"].map(|name| v[format!("cg_crosshair_{name}").as_str()].clone());
        assert_eq!(got, ["-147456", "283.5", "3"]);

        let rejected = [
            ("CSkSCHQKunGr2yPsJr2U3RTd8ycMA2pEs9jWnxK5KjMmWL", "garbled"),
            ("CSoQhciXBHZUKAbfexZBeMWFKxkrXL5MLumDLj3JYW4snP", "no screen height"),
            ("CSuZhVyU8icUwjTjopBSVi6ctEbq5GiOXJBRdC6LNLKaFL", "version 2"),
            ("CSGO-9JzcN-4dZtA-DdXis-8qz5T-rCnkP", "old CSGO"),
            ("CSvbPubOq37zTGqtsPTP5QTrp5CB4xFXiKRLfzJsm49ZR0", "never writes"),
            ("CSvbPubOq37zTGqtsPTP5QTrp5CB4xFXiKRLfzJsm49ZR", "44 letters"),
            ("CS99999999999999999999999999999999999999999999", "typo?"),
            (" \r\n", "no code"),
        ];
        for (code, why) in rejected {
            assert!(get(code).unwrap_err().contains(why), "{code}");
        }
    }

    #[test]
    fn misc() {
        assert!(same_address("Host.Example", "host.example:12203"));
        assert!(!same_address("host:12204", "host"));
        assert_eq!(parse_color("#00ff7F"), Some(0x00FF7F));
        assert_eq!(parse_color("12345"), None);
        assert_eq!(parse_color("GGGGGG"), None);
        assert_eq!(color_hex(0x0A0B0C), "0A0B0C");

        assert_eq!(mode_from_cvars("1", "1"), MODE_FULLSCREEN);
        assert_eq!(mode_from_cvars("0", "1"), MODE_BORDERLESS);
        assert_eq!(mode_from_cvars("0", "0"), MODE_WINDOW);
        let mut e = CvarEdits::new(&[("r_fullscreen", "1"), ("r_noborder", "0")]);
        mode_to_cvars(MODE_BORDERLESS, &mut e);
        assert_eq!((e.get("r_fullscreen"), e.get("r_noborder")), ("0", "1"));
        mode_to_cvars(MODE_FULLSCREEN, &mut e);
        assert_eq!((e.get("r_fullscreen"), e.get("r_noborder")), ("1", "1"));

        let r = |width, height| Resolution { width, height };
        let list = resolution_list(&[r(1920, 1080), r(1280, 720), r(320, 200)], Some(r(1920, 1080)));
        assert_eq!(list[0], r(1920, 1080));
        assert_eq!(list.iter().filter(|&&x| x == r(1920, 1080)).count(), 1);
        assert!(!list.contains(&r(2560, 1440)) && !list.contains(&r(320, 200)));
        // the numbered modes that fit, from 640 x 480 up: not 2048 x 1536 or 1600 x 1200 on a 1080p desktop, nor
        // 320 x 240, 400 x 300 and 512 x 384
        for (w, h) in [(1152, 864), (1024, 768), (960, 720), (856, 480), (800, 600)] {
            assert!(list.contains(&r(w, h)));
        }
        for (w, h) in [(2048, 1536), (1600, 1200), (512, 384), (400, 300), (320, 240)] {
            assert!(!list.contains(&r(w, h)));
        }
        assert_eq!(list.last(), Some(&r(640, 480)));

        let mut e = CvarEdits::new(&[("r_mode", "-2"), ("r_customwidth", "1600"), ("r_customheight", "1024")]);
        let sizes = [r(1920, 1080), r(1280, 720)];
        let desktop = Some(r(1920, 1080));
        // the desktop's resolution is the desktop's size, and no entry of its own
        let (entries, k) = resolution_entries(&sizes, desktop, &e);
        assert_eq!((entries.len(), k, entries[k].label()), (2, 0, "1920 \u{00D7} 1080 (16:9)".to_string()));
        e.set("r_mode", "-1");
        let (entries, k) = resolution_entries(&sizes, desktop, &e);
        assert_eq!((entries.len(), k, entries[k].width), (3, 1, 1600));
        // a numbered mode is its size: the list's own entry, or one more where it belongs
        let sizes = resolution_list(&[], Some(r(1920, 1080)));
        e.set("r_mode", "6");
        let (entries, k) = resolution_entries(&sizes, desktop, &e);
        assert_eq!((entries.len(), entries[k].label()), (sizes.len(), "1024 \u{00D7} 768 (4:3)".to_string()));
        e.set("r_mode", "10");
        let (entries, k) = resolution_entries(&sizes, desktop, &e);
        assert_eq!((entries.len(), k, entries[k].label()), (sizes.len() + 1, 0, "2048 \u{00D7} 1536 (4:3)".to_string()));
        // below 640 x 480: 640 x 480, passed as a change
        e.set("r_mode", "1");
        e.applied();
        raise_small_resolution(&mut e);
        let (entries, k) = resolution_entries(&sizes, desktop, &e);
        assert_eq!((entries.len(), entries[k].label()), (sizes.len(), "640 \u{00D7} 480 (4:3)".to_string()));
        assert_eq!(e.changes().len(), 3);
        e.set("r_customwidth", "320");
        e.set("r_customheight", "200");
        e.applied();
        raise_small_resolution(&mut e);
        assert_eq!((e.get("r_mode"), e.get("r_customwidth"), e.get("r_customheight")), ("-1", "640", "480"));
        e.set("r_mode", "11");
        e.applied();
        raise_small_resolution(&mut e);
        assert!(e.changes().is_empty());
        e.set("r_mode", "11");
        let (entries, k) = resolution_entries(&sizes, desktop, &e);
        assert_eq!(entries[k].label(), "856 \u{00D7} 480 (16:9)");
        e.set("r_mode", "12");
        let (entries, k) = resolution_entries(&sizes, desktop, &e);
        assert_eq!((entries.len(), entries[k].label()), (sizes.len(), "1920 \u{00D7} 1080 (16:9)".to_string()));
        assert!(entries.iter().all(|e| !e.label().contains("r_mode") && !e.label().contains("Desktop")));

        assert_eq!(aspect_ratio(1920, 1080), "16:9");
        assert_eq!(aspect_ratio(1366, 768), "16:9");
        assert_eq!(aspect_ratio(1920, 1200), "16:10");
        assert_eq!(aspect_ratio(1280, 1024), "5:4");
        assert_eq!(aspect_ratio(3440, 1440), "21:9");
        assert_eq!(aspect_ratio(2560, 1080), "21:9");
        assert_eq!(aspect_ratio(5120, 1440), "32:9");
        assert_eq!(aspect_ratio(1000, 1000), "1:1");
    }

    #[test]
    fn finds_configs() {
        let root = std::env::temp_dir().join(format!("mohl-core-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let (home, base) = (root.join("home"), root.join("base"));
        let write = |p: PathBuf, text: &str| {
            std::fs::create_dir_all(p.parent().unwrap()).unwrap();
            std::fs::write(p, text).unwrap();
        };
        let aa = home.join("main/configs/omconfig.cfg");
        write(aa.clone(), "seta name a\n");
        write(base.join("maintt/configs/unnamedsoldier.cfg"), "seta name b\n");
        let homes = [home.clone(), base.clone()];
        assert_eq!(find_config(&homes, 0), Some(aa.clone()));
        assert_eq!(find_config(&homes, 1), Some(aa.clone()));
        assert_eq!(find_config(&homes, 2), Some(aa.clone()));
        let bt = home.join("maintt/configs/omconfig.cfg");
        write(bt.clone(), "seta name c\n");
        assert_eq!(find_config(&homes, 2), Some(bt.clone()));
        assert_eq!(find_config(&[root.join("none")], 0), None);
        assert_eq!(read_config(&bt).unwrap()["name"], "c");
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn reads_and_keeps_settings() {
        // the macOS launcher's launcher.json reads the same
        let s = Settings::from_json(
            r#"{"game": 1, "slots": [{"address": "a:1", "servers": [{"label": "", "address": "a:1"}, {"address": " "}]}]}"#,
        );
        assert_eq!(s.game, 1);
        assert_eq!(s.slots.len(), GAME_COUNT);
        assert_eq!(s.slots[0].servers, vec![Server { label: "a:1".into(), address: "a:1".into(), ..Default::default() }]);
        assert_eq!(Settings::from_json("not json").slots.len(), GAME_COUNT);

        let mut s = Settings::from_json("{}");
        s.remember_server(0, "host", "pw", "");
        s.remember_server(0, "HOST:12203", "pw2", "r");
        assert_eq!(s.slots[0].servers.len(), 1);
        assert_eq!((s.slots[0].servers[0].address.as_str(), s.slots[0].servers[0].password.as_str()), ("HOST:12203", "pw2"));
        let json = serde_json::to_string(&s).unwrap();
        assert!(json.contains("\"slots\"") && json.contains("\"label\"") && !json.contains("gameFolder"));
        assert!(!json.contains("\"name\"")); // a server has a label; the player's name is the game's
        // Close on start: off unless turned on, kept as closeOnStart (the macOS launcher's name too)
        assert!(!s.close_on_start && json.contains("\"closeOnStart\":false"));
        assert!(Settings::from_json(r#"{"closeOnStart": true}"#).close_on_start);
    }

    #[test]
    fn lists_the_builtin_servers_first() {
        let mut s = Settings::from_json("{}");
        s.remember_server(0, "a:1", "", "");
        let list = listed_servers(&s, 0);
        assert_eq!(list.len(), 2);
        assert!(list[0].builtin && list[0].saved.is_none());
        assert_eq!((list[0].server.label.as_str(), list[0].server.address.as_str()), ("Forte FFA", "151.80.234.90:12203"));
        assert_eq!((list[1].server.address.as_str(), list[1].saved), ("a:1", Some(0)));
        // joined with a password: listed once, with it
        s.remember_server(0, "151.80.234.90:12203", "pw", "");
        let list = listed_servers(&s, 0);
        assert_eq!(list.len(), 2);
        assert_eq!((list[0].saved, list[0].server.password.as_str()), (Some(1), "pw"));
        // the other games don't have it
        assert!(listed_servers(&s, 1).is_empty());
    }

    #[test]
    fn plain_paths() {
        let plain = |p: &str| plain_path(Path::new(p)).to_str().unwrap().to_string();
        assert_eq!(plain(r"\\?\C:\Games\MOHAA"), r"C:\Games\MOHAA");
        assert_eq!(plain(r"\\?\d:\"), r"d:\");
        assert_eq!(plain(r"\\?\UNC\server\share\MOHAA"), r"\\server\share\MOHAA");
        assert_eq!(plain(r"C:\Games\MOHAA"), r"C:\Games\MOHAA");
        assert_eq!(plain(r"\\?\Volume{0}\MOHAA"), r"\\?\Volume{0}\MOHAA");
        assert_eq!(plain("/home/player/Games/MOHAA"), "/home/player/Games/MOHAA");
    }
}
