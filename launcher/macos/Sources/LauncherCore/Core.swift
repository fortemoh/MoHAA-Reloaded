// The launcher's logic, without UI, as launcher/desktop/src/core.cpp has it for Windows and Linux: the game's
// config file, the settings the launcher edits, the game's command line, the crosshair's geometry and CS2's
// crosshair codes.
import Foundation

public enum Game: Int, CaseIterable, Identifiable, Codable {
    case alliedAssault = 0, spearhead, breakthrough

    public var id: Int { rawValue }
    public var title: String { ["Allied Assault", "Spearhead", "Breakthrough"][rawValue] }
    /// The game's data folder, which is also its folder for configs
    public var dataDir: String { ["main", "mainta", "maintt"][rawValue] }
}

public typealias Cvars = [String: String] // lowercase name -> value

/// The values a config's `seta`/`set` lines leave, as the game reads them: quotes, `;` and `//` as the game's own
/// command parser handles them. Names are lowercased (cvar names don't depend on case).
public func parseConfig(_ text: String) -> Cvars {
    var out = Cvars()
    var tokens: [String] = []
    let b = Array(text.utf8)
    let n = b.count
    var i = 0
    var starComment = false

    func takeCommand() {
        defer { tokens.removeAll() }
        guard tokens.count >= 3 else { return }
        let cmd = tokens[0].lowercased()
        guard ["set", "seta", "sets", "setu"].contains(cmd) else { return }
        out[tokens[1].lowercased()] = tokens[2...].joined(separator: " ")
    }
    func at(_ k: Int) -> UInt8 { k < n ? b[k] : 0 }
    let quote = UInt8(ascii: "\""), slash = UInt8(ascii: "/"), star = UInt8(ascii: "*")
    let newline = UInt8(ascii: "\n"), cr = UInt8(ascii: "\r"), semicolon = UInt8(ascii: ";"), space = UInt8(ascii: " ")

    while i < n {
        let c = b[i]
        if starComment {
            if c == star && at(i + 1) == slash {
                starComment = false
                i += 2
            } else {
                i += 1
            }
            continue
        }
        if c == newline || c == cr || c == semicolon {
            // the end of a command (the game's command buffer splits there outside quotes)
            takeCommand()
            i += 1
            continue
        }
        if c <= space {
            i += 1
            continue
        }
        if c == slash && at(i + 1) == slash {
            while i < n && b[i] != newline && b[i] != cr { i += 1 }
            continue
        }
        if c == slash && at(i + 1) == star {
            starComment = true
            i += 2
            continue
        }
        var token: [UInt8] = []
        if c == quote {
            // a quoted token runs to the closing quote or the end of the line
            i += 1
            while i < n && b[i] != quote && b[i] != newline && b[i] != cr {
                token.append(b[i])
                i += 1
            }
            if i < n && b[i] == quote { i += 1 }
        } else {
            while i < n && b[i] > space && b[i] != quote && b[i] != semicolon
                && !(b[i] == slash && (at(i + 1) == slash || at(i + 1) == star)) {
                token.append(b[i])
                i += 1
            }
        }
        tokens.append(String(decoding: token, as: UTF8.self))
    }
    takeCommand()
    return out
}

public func readConfig(_ url: URL) -> Cvars? {
    guard let data = try? Data(contentsOf: url) else { return nil }
    return parseConfig(String(decoding: data, as: UTF8.self))
}

/// The first config that exists: for the game's own data folder, then for Allied Assault's (an expansion's first
/// start inherits it), in each home folder in turn, `omconfig.cfg` before the original game's `unnamedsoldier.cfg`.
public func findConfig(homes: [URL], game: Game) -> URL? {
    var dirs = [game.dataDir]
    if game != .alliedAssault { dirs.append(Game.alliedAssault.dataDir) }
    for name in ["omconfig.cfg", "unnamedsoldier.cfg"] {
        for dir in dirs {
            for home in homes {
                let url = home.appendingPathComponent(dir).appendingPathComponent("configs").appendingPathComponent(name)
                var isDir: ObjCBool = false
                if FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir) && !isDir.boolValue {
                    return url
                }
            }
        }
    }
    return nil
}

/// The settings the launcher edits. Each holds the value the game's config had when it was last read and the one
/// shown in the launcher; only the ones the player changed in the launcher go to the game, so a change made in the
/// game (its menus, the console) is never undone by starting it from here.
public struct CvarEdits: Equatable {
    struct Entry: Equatable {
        /// lowercase, to find it (cvar names don't depend on case)
        let key: String
        /// as written, to pass it
        let name: String
        let def: String
        var value: String
        var loaded: String
    }
    private var entries: [Entry]

    public init(defaults: [(String, String)]) {
        entries = defaults.map { Entry(key: $0.0.lowercased(), name: $0.0, def: $0.1, value: $0.1, loaded: $0.1) }
    }

    public func get(_ name: String) -> String {
        entries.first { $0.key == name.lowercased() }?.value ?? ""
    }

    public mutating func set(_ name: String, _ value: String) {
        if let k = entries.firstIndex(where: { $0.key == name.lowercased() }) {
            entries[k].value = value
        }
    }

    public func changed(_ name: String) -> Bool {
        guard let e = entries.first(where: { $0.key == name.lowercased() }) else { return false }
        return e.value != e.loaded
    }

    /// Takes the config's values (the defaults for the ones it doesn't have); a value changed in the launcher stays
    public mutating func load(_ config: Cvars) {
        for k in entries.indices {
            let changed = entries[k].value != entries[k].loaded
            entries[k].loaded = config[entries[k].key] ?? entries[k].def
            if !changed { entries[k].value = entries[k].loaded }
        }
    }

    /// The changed values, in the order of the defaults, by their names as written
    public var changes: [(String, String)] {
        entries.filter { $0.value != $0.loaded }.map { ($0.name, $0.value) }
    }

    /// The game was started with the changes: they are the config's values now
    public mutating func applied() {
        for k in entries.indices { entries[k].loaded = entries[k].value }
    }
}

/// One value on the game's command line. The game joins its arguments into one line, quoting the ones with a space,
/// and splits that line into commands at each `+` and `;` outside quotes: a value is cleaned of quotes and control
/// characters, and quoted when it has none of its own (no space) but could be split or cut (`+`, `;`, `//`).
public func engineArg(_ value: String) -> String {
    var v = ""
    for scalar in value.unicodeScalars {
        if scalar == "\t" {
            v.append(" ")
        } else if scalar != "\"" && scalar.value >= 0x20 && scalar.value != 0x7f {
            v.unicodeScalars.append(scalar)
        }
    }
    if v.contains(" ") { return v } // the game puts it in quotes itself
    if v.isEmpty || v.contains("+") || v.contains(";") || v.contains("//") || v.contains("/*") {
        return "\"" + v + "\""
    }
    return v
}

public struct Launch {
    /// fs_homepath: where the game writes everything (the game folder), if not empty
    public var homepath = ""
    /// fs_basepath: the game folder when the game isn't in it, else empty
    public var basepath = ""
    public var game: Game = .alliedAssault
    public var cvars: [(String, String)] = []
    public var address = ""
    public var password = ""
    public var rcon = ""
    public init() {}
}

/// The game's arguments: +set fs_homepath, +set fs_basepath, +set com_target_game N, the cvars, the passwords,
/// +connect
public func buildArgs(_ launch: Launch) -> [String] {
    var args: [String] = []
    func set(_ name: String, _ value: String) { args += ["+set", name, engineArg(value)] }
    if !launch.homepath.isEmpty { set("fs_homepath", launch.homepath) }
    if !launch.basepath.isEmpty { set("fs_basepath", launch.basepath) }
    set("com_target_game", String(launch.game.rawValue))
    for (name, value) in launch.cvars { set(name, value) }
    if !launch.password.isEmpty { set("password", launch.password) }
    if !launch.rcon.isEmpty { set("rconPassword", launch.rcon) }
    if !launch.address.isEmpty { args += ["+connect", engineArg(launch.address)] }
    return args
}

public func trimmed(_ s: String) -> String {
    s.trimmingCharacters(in: .whitespacesAndNewlines)
}

public func sameAddress(_ a: String, _ b: String) -> Bool {
    func key(_ s: String) -> String {
        let k = trimmed(s).lowercased()
        return k.contains(":") ? k : k + ":12203" // the game's default port
    }
    return key(a) == key(b)
}

/// "FFFFFF", "#ffffff" -> 0xFFFFFF; nil if it isn't 6 hex digits (the game then draws white)
public func parseColor(_ text: String) -> UInt32? {
    var hex = trimmed(text)
    if hex.hasPrefix("#") { hex.removeFirst() }
    guard hex.count == 6, hex.allSatisfy({ $0.isHexDigit }) else { return nil }
    return UInt32(hex, radix: 16)
}

public func colorHex(_ rgb: UInt32) -> String {
    String(format: "%06X", rgb & 0xFFFFFF)
}

public struct Box: Equatable {
    public var x, y, w, h: Int
}

/// The crosshair's settings that shape it: sizes in pixels at 1080 lines, the outline 0 (none), 1 (full) or 2 (half)
public struct Crosshair {
    public var length, gap, thickness: Double
    public var outline: Int
    public var dot, t: Bool

    public init(length: Double, gap: Double, thickness: Double, outline: Int = 0, dot: Bool = false, t: Bool = false) {
        self.length = length
        self.gap = gap
        self.thickness = thickness
        self.outline = outline
        self.dot = dot
        self.t = t
    }
}

/// The crosshair as cg_drawtools.cpp's CG_UpdateCrosshair lays it out at 1080 lines (CS2's static cross), in HUD
/// pixels from the screen's middle pixel
public struct CrossLayout {
    /// the arms and the dot, joined into boxes that don't overlap
    public var boxes: [Box]
    /// the outline, drawn first, joined the same way, less the pieces
    public var outline: [Box]
    /// the square around the crosshair's middle that holds it all: its width (and height), and its top left corner
    /// (the same both ways)
    public var extent: Int
    public var start: Int
}

public func crosshairBoxes(_ c: Crosshair) -> CrossLayout {
    // in whole pixels as the game computes them, at 1080 lines: half away from zero, at least 1 above 0, else 0
    func pixels(_ value: Double) -> Int {
        let v = Float(value)
        guard v > 0 else { return 0 } // NaN too
        return max(Int(min(v, 4096) * 1080 / 1080 + 0.5), 1)
    }
    let l = pixels(c.length), t = pixels(c.thickness), g = pixels(c.gap)
    // a line t thick has (t + 1) / 2 pixels before the middle one; the gap counts from it, a pixel less after it
    // when t is odd
    let before = (t + 1) / 2, odd = t & 1
    var pieces: [Box] = []
    if t > 0 && l > 0 {
        pieces += [Box(x: -g - l, y: -before, w: l, h: t), Box(x: g - odd, y: -before, w: l, h: t),
                   Box(x: -before, y: g - odd, w: t, h: l)]
        if !c.t {
            pieces.append(Box(x: -before, y: -g - l, w: t, h: l))
        }
    }
    if t > 0 && c.dot {
        pieces.append(Box(x: -before, y: -before, w: t, h: t))
    }
    // a pixel above (over the corners) and left of every piece; full: below and right too; nowhere under a piece
    let mode = min(max(c.outline, 0), 2)
    var strips: [Box] = []
    for p in pieces where mode != 0 {
        strips += [Box(x: p.x - 1, y: p.y - 1, w: p.w + (mode == 1 ? 2 : 1), h: 1), Box(x: p.x - 1, y: p.y, w: 1, h: p.h)]
        if mode == 1 {
            strips += [Box(x: p.x - 1, y: p.y + p.h, w: p.w + 2, h: 1), Box(x: p.x + p.w, y: p.y, w: 1, h: p.h)]
        }
    }
    // the middle is -odd / 2 both ways: the farthest edge from it, twice, is the square's width
    let extent = (pieces + strips).flatMap { [$0.x, $0.x + $0.w, $0.y, $0.y + $0.h] }.map { abs(2 * $0 + odd) }.max() ?? 0
    return CrossLayout(boxes: crosshairUnion(pieces, minus: []), outline: crosshairUnion(strips, minus: pieces),
                       extent: extent, start: -((extent + odd) / 2))
}

/// The pixels of boxes less those of others, as boxes that don't overlap, as CG_CrosshairUnion joins them (at most
/// 64): in each band between the boxes' top and bottom edges, the first boxes' spans less the others', a box going on
/// down into the next band when it has the same span there. A see-through crosshair blends each pixel once, and the
/// outline nowhere under a piece
func crosshairUnion(_ boxes: [Box], minus: [Box]) -> [Box] {
    // the boxes' spans across the band y0 to y1, sorted and joined where they touch
    func spans(_ boxes: [Box], _ y0: Int, _ y1: Int) -> [(Int, Int)] {
        let all = boxes.filter { $0.y <= y0 && $0.y + $0.h >= y1 && $0.w > 0 }.map { ($0.x, $0.x + $0.w) }
        var joined: [(Int, Int)] = []
        for (x0, x1) in all.sorted(by: { $0.0 < $1.0 }) {
            if let last = joined.last, x0 <= last.1 {
                joined[joined.count - 1].1 = max(last.1, x1)
            } else {
                joined.append((x0, x1))
            }
        }
        return joined
    }
    let edges = (boxes + minus).flatMap { [$0.y, $0.y + $0.h] }.sorted()
    var out: [Box] = []
    for k in edges.indices.dropLast() where edges[k] != edges[k + 1] {
        let y0 = edges[k], y1 = edges[k + 1], above = out.count
        let cuts = spans(minus, y0, y1)
        for (s0, s1) in spans(boxes, y0, y1) {
            // the span up to each cut, then on from its end
            var x0 = s0
            for j in 0...cuts.count {
                if x0 >= s1 { break }
                let x1 = j < cuts.count ? min(max(cuts[j].0, x0), s1) : s1
                if x1 > x0 {
                    if let i = out[..<above].firstIndex(where: { $0.x == x0 && $0.w == x1 - x0 && $0.y + $0.h == y0 }) {
                        out[i].h += y1 - y0
                    } else if out.count < 64 {
                        out.append(Box(x: x0, y: y0, w: x1 - x0, h: y1 - y0))
                    }
                }
                if j < cuts.count {
                    x0 = max(x0, cuts[j].1)
                }
            }
        }
    }
    return out
}

/// The characters of CS2's crosshair codes, a digit of base 57 each
private let codeDigits = Array("ABCDEFGHJKLMNOPQRSTUVWXYZabcdefhijkmnopqrstuvwxyz23456789".utf8)

/// The settings a CS2 crosshair code sets, as the game's crosshair_import sets them (code/client/cl_crosshair.c,
/// which describes the format), from the text's first word; else why it isn't a code CS2 writes now
public func crosshairImport(_ text: String) -> Result<[(String, String)], CrosshairCodeError> {
    // (by bytes: to Swift "\r\n" is one character)
    let code = Array(text.utf8).split(whereSeparator: { [32, 9, 13, 10].contains($0) }).first
        .map { String(decoding: $0, as: UTF8.self) } ?? ""
    if code.isEmpty {
        return .failure(.init("no code: copy the crosshair's code in CS2 first"))
    }
    if code.hasPrefix("CSGO") {
        return .failure(.init("an old CSGO- code: CS2 no longer takes those either, copy the crosshair's new code from CS2"))
    }
    let chars = Array(code.utf8)
    guard code.hasPrefix("CS"), chars.count == 46 else {
        return .failure(.init("not a crosshair code (CS and 44 letters and digits)"))
    }
    // a number in base 57, its last character the most significant digit, into 32 bytes, byte 0 the most significant
    var b = [UInt8](repeating: 0, count: 32)
    for c in chars.dropFirst(2).reversed() {
        guard let digit = codeDigits.firstIndex(of: c) else {
            return .failure(.init("not a crosshair code (a character CS2 never writes: a typo?)"))
        }
        var carry = UInt32(digit)
        for i in b.indices.reversed() {
            carry += UInt32(b[i]) * 57
            b[i] = UInt8(truncatingIfNeeded: carry)
            carry >>= 8
        }
        if carry != 0 {
            return .failure(.init("not a crosshair code (a typo?)"))
        }
    }
    if b[0] != b[1...].reduce(0, &+) {
        return .failure(.init("the code is garbled (its checksum doesn't match: a typo?)"))
    }
    if b[1] != 1 {
        return .failure(.init("a crosshair code of a newer kind (version \(b[1])) than this game reads"))
    }
    let height = UInt16(b[2]) | UInt16(b[3]) << 8
    if height == 0 {
        return .failure(.init("the code has no screen height"))
    }

    // sizes are pixels at the code's height (240 at least): at 1080 lines, in the game's float written as printf's %g
    let scale = Float(1080) / Float(max(height, 240))
    func size(_ pixels: Float) -> String { String(format: "%g", Double(pixels * scale)) }
    func hex(_ i: Int) -> String { String(format: "%02X%02X%02X", b[i], b[i + 1], b[i + 2]) }
    let dotOnly = b[4] & 31 == 6
    return .success([
        ("cg_crosshair_length", dotOnly ? "0" : size(Float(b[16]))),
        ("cg_crosshair_gap", size(Float(Int16(bitPattern: UInt16(b[14]) | UInt16(b[15]) << 8)))),
        ("cg_crosshair_thickness", size(Float(b[13] & 63))),
        ("cg_crosshair_color", hex(5)),
        ("cg_crosshair_alpha", String(b[8])),
        ("cg_crosshair_outline", String(b[13] >> 6)),
        ("cg_crosshair_outline_color", hex(9)),
        ("cg_crosshair_outline_alpha", String(b[12])),
        ("cg_crosshair_dot", dotOnly || b[4] & 64 != 0 ? "1" : "0"),
        ("cg_crosshair_t", String(b[4] >> 7)),
        ("cg_crosshair_code", code),
    ])
}

/// Why a text isn't a crosshair code CS2 writes now (in lower case: a sentence's middle)
public struct CrosshairCodeError: Error, Equatable {
    public let reason: String
    init(_ reason: String) { self.reason = reason }
}

/// The game's window modes on the Metal port, as r_fullscreen, r_noborder and r_borderlessWindow pick them
public enum DisplayMode: Int, CaseIterable, Identifiable {
    case space, exclusive, borderlessFullscreen, window, borderlessWindow

    public var id: Int { rawValue }
    public var title: String {
        ["Fullscreen Space", "Exclusive fullscreen", "Borderless fullscreen", "Window", "Borderless window"][rawValue]
    }
    /// The fullscreen Space and a borderless window over the screen take the desktop's size, whatever r_mode says
    public var usesResolution: Bool { self == .exclusive || self == .window || self == .borderlessWindow }

    public init(fullscreen: String, noborder: String, borderlessWindow: String) {
        let fs = (Int(fullscreen) ?? 0) != 0, nb = (Int(noborder) ?? 0) != 0, bw = (Int(borderlessWindow) ?? 0) != 0
        if fs {
            self = nb ? (bw ? .borderlessFullscreen : .space) : .exclusive
        } else {
            self = nb ? .borderlessWindow : .window
        }
    }

    public func apply(to edits: inout CvarEdits) {
        switch self {
        case .space:
            edits.set("r_fullscreen", "1"); edits.set("r_noborder", "1"); edits.set("r_borderlessWindow", "0")
        case .exclusive:
            edits.set("r_fullscreen", "1"); edits.set("r_noborder", "0")
        case .borderlessFullscreen:
            edits.set("r_fullscreen", "1"); edits.set("r_noborder", "1"); edits.set("r_borderlessWindow", "1")
        case .window:
            edits.set("r_fullscreen", "0"); edits.set("r_noborder", "0")
        case .borderlessWindow:
            edits.set("r_fullscreen", "0"); edits.set("r_noborder", "1")
        }
    }
}

public struct Resolution: Hashable {
    public var width: Int
    public var height: Int
    public init(_ width: Int, _ height: Int) {
        self.width = width
        self.height = height
    }
}

/// The game's numbered modes, r_mode 0 to 11 (the renderers' r_vidModes): a config can hold one
public let rModeSizes: [Resolution] = [
    (320, 240), (400, 300), (512, 384), (640, 480), (800, 600), (960, 720), (1024, 768), (1152, 864), (1280, 1024),
    (1600, 1200), (2048, 1536), (856, 480),
].map { Resolution($0.0, $0.1) }

/// The smallest size the launcher offers or passes
public let minResolution = Resolution(640, 480)

/// A config's size below 640 x 480 (r_mode 0 to 2, or a custom size) as 640 x 480: the menu shows it, and starting
/// the game passes it, as a change made in the launcher
public func raiseSmallResolution(_ edits: inout CvarEdits) {
    let mode = Int(edits.get("r_mode")) ?? -2
    var size: Resolution?
    if mode == -1 {
        size = Resolution(Int(edits.get("r_customwidth")) ?? 0, Int(edits.get("r_customheight")) ?? 0)
    } else if rModeSizes.indices.contains(mode) {
        size = rModeSizes[mode]
    }
    if let size, size.width < minResolution.width || size.height < minResolution.height {
        edits.set("r_mode", "-1")
        edits.set("r_customwidth", String(minResolution.width))
        edits.set("r_customheight", String(minResolution.height))
    }
}

/// A size's aspect ratio, by the common name it's within 3% of (1366 x 768 is 16:9), else as a reduced fraction
public func aspectRatio(_ width: Int, _ height: Int) -> String {
    guard width > 0, height > 0 else { return "" }
    let named = [(4, 3), (5, 4), (3, 2), (16, 10), (16, 9), (21, 9), (32, 9)]
    let ratio = Double(width) / Double(height)
    if let match = named.first(where: { abs(ratio / (Double($0.0) / Double($0.1)) - 1) < 0.03 }) {
        return "\(match.0):\(match.1)"
    }
    var (a, b) = (width, height)
    while b != 0 { (a, b) = (b, a % b) }
    return "\(width / a):\(height / a)"
}

/// The common sizes, largest first: the ultrawides, 4K, QHD and 16:10, 1080p, then the 4:3 and 5:4 ones played
/// stretched
public let commonResolutions: [Resolution] = [
    (5120, 1440), (3840, 2160), (3440, 1440), (2560, 1600), (2560, 1440), (2560, 1080), (1920, 1200), (1920, 1080),
    (1680, 1050), (1600, 1200), (1600, 900), (1440, 1080), (1440, 900), (1366, 768), (1280, 1024), (1280, 960),
    (1280, 720), (1152, 864), (1024, 768), (960, 720), (800, 600), (640, 480),
].map { Resolution($0.0, $0.1) }

/// The sizes to offer: the screen's own and the common ones that fit it, largest first, without duplicates. Exclusive
/// fullscreen scales any size to the screen, so the display's own modes don't matter, and nothing larger than the
/// screen is offered (the game refuses it in fullscreen). A config's own size, if it isn't one of them, is added to
/// the menu by the app.
public func resolutionList(desktop: Resolution) -> [Resolution] {
    var list: [Resolution] = []
    func add(_ r: Resolution) {
        if r.width >= minResolution.width && r.height >= minResolution.height && !list.contains(r) { list.append(r) }
    }
    add(desktop)
    for r in commonResolutions where desktop.width <= 0 || (r.width <= desktop.width && r.height <= desktop.height) {
        add(r)
    }
    return list.sorted { $0.width != $1.width ? $0.width > $1.width : $0.height > $1.height }
}

/// r_fullscreenStretch: how exclusive fullscreen shows a size other than the screen's. Fit keeps its shape, as large as
/// it fits, between black bars if the shape differs; Stretch fills the screen; 1:1 doesn't scale it, centered between
/// black bars.
public enum FullscreenScaling: Int, CaseIterable, Identifiable {
    case fit, stretch, oneToOne

    public var id: Int { rawValue }
    public var title: String { ["Fit", "Stretch", "1:1"][rawValue] }

    /// As the game reads it: 2 is 1:1, and any other number but 0 is stretched
    public init(cvar: String) {
        switch Int(cvar) ?? 0 {
        case 0: self = .fit
        case 2: self = .oneToOne
        default: self = .stretch
        }
    }

    public func apply(to edits: inout CvarEdits) {
        edits.set("r_fullscreenStretch", String(rawValue))
    }
}

/// The screen's own size has nothing to scale (an unknown screen: it may matter)
public func scalingMatters(_ size: Resolution, desktop: Resolution) -> Bool {
    desktop.width <= 0 || desktop.height <= 0 || size != desktop
}

/// A data folder (main, mainta, maintt), in any case: retail folders sometimes come as MAIN or MainTA
public func dataFolder(_ folder: URL, _ name: String) -> URL? {
    let fm = FileManager.default
    var isDir: ObjCBool = false
    let exact = folder.appendingPathComponent(name)
    if fm.fileExists(atPath: exact.path, isDirectory: &isDir) && isDir.boolValue { return exact }
    let names = (try? fm.contentsOfDirectory(atPath: folder.path)) ?? []
    guard let match = names.first(where: { $0.caseInsensitiveCompare(name) == .orderedSame }) else { return nil }
    let url = folder.appendingPathComponent(match)
    return fm.fileExists(atPath: url.path, isDirectory: &isDir) && isDir.boolValue ? url : nil
}

/// The retail game's data: main/Pak0.pk3, in any case
public func hasGameData(_ folder: URL) -> Bool {
    guard let main = dataFolder(folder, "main") else { return false }
    let names = (try? FileManager.default.contentsOfDirectory(atPath: main.path)) ?? []
    return names.contains { $0.caseInsensitiveCompare("Pak0.pk3") == .orderedSame }
}

/// A server in the list: what it's called there (its address until it's renamed), its address and the passwords
/// used on it. The player's own name isn't part of it: that's the game's.
public struct Server: Codable, Identifiable, Hashable {
    public var id = UUID()
    public var label: String
    public var address: String
    public var password = ""
    public var rcon = ""

    enum CodingKeys: String, CodingKey { case label, address, password, rcon }

    public init(label: String, address: String, password: String = "", rcon: String = "") {
        self.label = label
        self.address = address
        self.password = password
        self.rcon = rcon
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        address = try c.decodeIfPresent(String.self, forKey: .address) ?? ""
        label = try c.decodeIfPresent(String.self, forKey: .label) ?? ""
        if trimmed(label).isEmpty { label = address }
        password = try c.decodeIfPresent(String.self, forKey: .password) ?? ""
        rcon = try c.decodeIfPresent(String.self, forKey: .rcon) ?? ""
    }
}

/// Per game: the server fields and the servers played on
public struct GameSlot: Codable {
    public var address = ""
    public var password = ""
    public var rcon = ""
    public var servers: [Server] = []

    public init() {}

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        address = try c.decodeIfPresent(String.self, forKey: .address) ?? ""
        password = try c.decodeIfPresent(String.self, forKey: .password) ?? ""
        rcon = try c.decodeIfPresent(String.self, forKey: .rcon) ?? ""
        servers = (try c.decodeIfPresent([Server].self, forKey: .servers) ?? []).filter { !trimmed($0.address).isEmpty }
    }
}

/// The launcher's own settings, as the game folder's mohlauncher.json holds them (the Windows and Linux launcher's
/// are the same)
public struct LauncherSettings: Codable {
    public var game = Game.alliedAssault
    public var slots = [GameSlot](repeating: GameSlot(), count: Game.allCases.count)
    /// Quit the launcher once the game has started (Single Player or Connect)
    public var closeOnStart = false

    public init() {}

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        game = (try? c.decodeIfPresent(Game.self, forKey: .game)) ?? .alliedAssault
        slots = (try? c.decodeIfPresent([GameSlot].self, forKey: .slots)) ?? []
        while slots.count < Game.allCases.count { slots.append(GameSlot()) }
        closeOnStart = (try? c.decodeIfPresent(Bool.self, forKey: .closeOnStart)) ?? false
    }

    public static func from(json data: Data?) -> LauncherSettings {
        data.flatMap { try? JSONDecoder().decode(LauncherSettings.self, from: $0) } ?? LauncherSettings()
    }

    /// Keeps the server in the game's list (added at the end), with the passwords used
    public mutating func remember(_ game: Game, address: String, password: String, rcon: String) {
        if let k = slots[game.rawValue].servers.firstIndex(where: { sameAddress($0.address, address) }) {
            slots[game.rawValue].servers[k].address = address
            slots[game.rawValue].servers[k].password = password
            slots[game.rawValue].servers[k].rcon = rcon
        } else {
            slots[game.rawValue].servers.append(Server(label: address, address: address, password: password, rcon: rcon))
        }
    }
}

/// The servers the launcher comes with, first in their game's list: they can't be renamed or removed
public let builtinServers: [(game: Game, label: String, address: String)] = [
    (.alliedAssault, "Forte FFA", "151.80.234.90:12203"),
]

/// A server as the list shows it
public struct Listed: Identifiable {
    /// with the passwords last used on it
    public var server: Server
    /// its place in the game's saved servers, if it's there
    public var saved: Int?
    public var builtin: Bool
    public var id: String { builtin ? "builtin:" + server.address : server.id.uuidString }
}

/// The game's list: its built-in servers, then the ones the player joined (a built-in one's entry, which keeps the
/// passwords used, isn't listed twice)
public func listedServers(_ settings: LauncherSettings, _ game: Game) -> [Listed] {
    let saved = settings.slots[game.rawValue].servers
    var list: [Listed] = []
    for b in builtinServers where b.game == game {
        let k = saved.firstIndex { sameAddress($0.address, b.address) }
        let server = Server(label: b.label, address: b.address, password: k.map { saved[$0].password } ?? "",
                            rcon: k.map { saved[$0].rcon } ?? "")
        list.append(Listed(server: server, saved: k, builtin: true))
    }
    for (k, s) in saved.enumerated()
    where !builtinServers.contains(where: { $0.game == game && sameAddress($0.address, s.address) }) {
        list.append(Listed(server: s, saved: k, builtin: false))
    }
    return list
}
