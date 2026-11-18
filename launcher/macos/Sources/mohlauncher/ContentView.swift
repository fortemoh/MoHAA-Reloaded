// The launcher's window, as the Windows and Linux launcher's (launcher/desktop/ui/app.slint) with macOS's own
// controls: in one card where to play on the left (the game, the server, the servers played on) and the player's
// settings on the right (the name, the passwords, the display); under it the buttons.
//
// One spacing system throughout (Metrics): every field is a block of a label over its control (labelGap apart),
// and blocks, cards and sections are `gap` apart, with `gap` of padding inside each card and around the window.
import AppKit
import LauncherCore
import SwiftUI

enum Metrics {
    static let gap: CGFloat = 12
    static let labelGap: CGFloat = 4
    static let labelHeight: CGFloat = 16
    static let control: CGFloat = 32 // a control's height, where rows must line up (as on Windows and Linux)
    static let row: CGFloat = 34
    static let field: CGFloat = labelHeight + labelGap + control // a field's block: its label over its control
}

extension Color {
    static let base = Color(red: 14 / 255, green: 14 / 255, blue: 14 / 255) // near black
    static let card = Color.white.opacity(0.035)
    static let stroke = Color.white.opacity(0.07)
    static let hoverFill = Color.white.opacity(0.06)
    static let selectedFill = Color.white.opacity(0.09)
    static let warning = Color(red: 252 / 255, green: 195 / 255, blue: 61 / 255)
    static let discord = Color(red: 88 / 255, green: 101 / 255, blue: 242 / 255) // Discord's blurple
}

struct ContentView: View {
    @EnvironmentObject var model: LauncherModel

    var body: some View {
        VStack(spacing: Metrics.gap) {
            Card {
                PlayerAndServers()
            }
            Footer()
        }
        .padding(Metrics.gap)
        .frame(width: 660)
        .fixedSize(horizontal: false, vertical: true)
        .background(Color.base)
        .background(WindowStyler())
        .preferredColorScheme(.dark)
        .onReceive(NSWorkspace.shared.notificationCenter.publisher(for: NSWorkspace.didTerminateApplicationNotification)) {
            model.applicationTerminated($0.userInfo?[NSWorkspace.applicationUserInfoKey] as? NSRunningApplication)
        }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            model.tick()
        }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.willTerminateNotification)) { _ in
            model.save()
        }
        .sheet(isPresented: Binding(get: { model.updateDialog != .none }, set: { _ in })) {
            UpdateSheet().environmentObject(model)
        }
        .alert("Where is MOHAA?", isPresented: $model.askForFolder) {
            Button("Choose Folder…") {
                Task { @MainActor in model.chooseGameFolder() }
            }
            Button("Not Now", role: .cancel) {}
        } message: {
            Text("Choose the folder Medal of Honor: Allied Assault is installed in: the one with main/ (Pak0.pk3 to Pak6.pk3), and mainta/ and maintt/ for the expansions.")
        }
    }
}

/// The window's title bar in the window's own near black
struct WindowStyler: NSViewRepresentable {
    func makeNSView(context: Context) -> NSView {
        let view = NSView()
        Task { @MainActor in
            guard let window = view.window else { return }
            window.titlebarAppearsTransparent = true
            window.backgroundColor = NSColor(srgbRed: 14 / 255, green: 14 / 255, blue: 14 / 255, alpha: 1)
        }
        return view
    }

    func updateNSView(_ nsView: NSView, context: Context) {}
}

struct Card<Content: View>: View {
    let content: Content

    init(@ViewBuilder content: () -> Content) {
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: Metrics.gap) {
            content
        }
        .padding(Metrics.gap)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(RoundedRectangle(cornerRadius: 8).fill(Color.card))
        .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(Color.stroke))
    }
}

struct FieldLabel: View {
    let text: String

    var body: some View {
        Text(text)
            .font(.system(size: 12))
            .foregroundStyle(.secondary)
            .frame(height: Metrics.labelHeight)
    }
}

/// A field: its label (and an action on the right of it) over its control
struct Field<Accessory: View, Control: View>: View {
    let label: String
    let accessory: Accessory
    let control: Control

    init(_ label: String, @ViewBuilder accessory: () -> Accessory, @ViewBuilder control: () -> Control) {
        self.label = label
        self.accessory = accessory()
        self.control = control()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: Metrics.labelGap) {
            HStack(spacing: 0) {
                FieldLabel(text: label)
                Spacer(minLength: 8)
                accessory
            }
            .frame(height: Metrics.labelHeight)
            control
        }
    }
}

extension Field where Accessory == EmptyView {
    init(_ label: String, @ViewBuilder control: () -> Control) {
        self.init(label, accessory: { EmptyView() }, control: control)
    }
}

/// A small action in a field's label row, on the right: an optional symbol and a word
struct LabelAction: View {
    let systemImage: String?
    let title: String
    var active = true // in the accent color, or (a toggle that's off) in the labels' color
    var enabled = true
    @State var hover = false
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 4) {
                if let systemImage {
                    Image(systemName: systemImage).font(.system(size: 10, weight: .semibold))
                }
                Text(title).font(.system(size: 12))
            }
            .foregroundStyle(!enabled ? Color.secondary.opacity(0.5)
                : hover ? Color.primary : active ? Color.accentColor : Color.secondary)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .onHover { hover = $0 }
    }
}

/// The games, each its icon with its abbreviation beside it (the name when hovered), as tall as a field (label and
/// control) beside them
struct GameTabs: View {
    var body: some View {
        HStack(spacing: Metrics.gap / 2) {
            ForEach(Game.allCases) { game in
                GameTab(game: game)
            }
        }
        .frame(height: Metrics.field)
    }
}

struct GameTab: View {
    @EnvironmentObject var model: LauncherModel
    let game: Game
    @State var hover = false

    var body: some View {
        let selected = model.game == game
        let available = model.available(game)
        Button {
            model.select(game)
        } label: {
            HStack(spacing: 8) {
                // the game's icon at its own size (32 points: the 64-pixel picture at 2x), straight on the tab: the
                // tab's fill, its edge and the bold letters show the selection
                Image(nsImage: model.icon(for: game))
                    .resizable()
                    .interpolation(.high)
                    .frame(width: 32, height: 32)
                Text(game.short)
                    .font(.system(size: 16, weight: selected ? .semibold : .regular))
                    .lineLimit(1)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(RoundedRectangle(cornerRadius: 6)
                .fill(selected ? Color.selectedFill : hover ? Color.hoverFill : Color.black.opacity(0.19)))
            .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(Color.white.opacity(selected ? 0.1 : 0.04)))
            .contentShape(RoundedRectangle(cornerRadius: 6))
        }
        .buttonStyle(.plain)
        .disabled(!available)
        .opacity(available ? 1 : 0.4)
        .onHover { hover = $0 }
        .help(available ? game.title : "\(game.title)'s data (\(game.dataDir)/) isn't in the game folder")
    }
}

/// Two columns: the game, the server and the servers played on; the player's name, the passwords and the display
struct PlayerAndServers: View {
    @EnvironmentObject var model: LauncherModel

    var body: some View {
        VStack(spacing: Metrics.gap) {
            Grid(horizontalSpacing: Metrics.gap, verticalSpacing: Metrics.gap) {
                GridRow {
                    GameTabs()
                    Field("Player name", accessory: {
                        LabelAction(systemImage: model.discordPresence ? "checkmark.square.fill" : "square",
                                    title: "Discord presence", active: model.discordPresence) {
                            model.discordPresence.toggle()
                        }
                        .help("Show what you play on your Discord profile while Discord runs (cl_discordPresence)")
                    }) {
                        TextField("Player name", text: model.name, prompt: Text("UnnamedSoldier"))
                    }
                }
                GridRow {
                    Field("Server", accessory: {
                        LabelAction(systemImage: model.bookmarked ? "bookmark.fill" : "bookmark",
                                    title: model.bookmarked ? "Bookmarked" : "Bookmark",
                                    enabled: model.canBookmark) {
                            model.bookmark()
                        }
                        .help(model.bookmarked ? "In the list: click to keep its passwords too"
                              : !trimmed(model.address).isEmpty && !model.canBookmark
                              ? "The list is full (\(listRows) servers): remove one first"
                              : "Keep it in the list (only bookmarked servers are kept)")
                    }) {
                        TextField("Server", text: $model.address, prompt: Text("host:port"))
                            .onSubmit { model.start(connect: true) }
                    }
                    // the server's two passwords, half the column each
                    HStack(spacing: Metrics.gap) {
                        Field("Server password") {
                            SecureField("Server password", text: $model.password, prompt: Text("If it has one"))
                                .onSubmit { model.start(connect: true) }
                        }
                        .frame(maxWidth: .infinity)
                        Field("RCON password") {
                            SecureField("RCON password", text: $model.rcon, prompt: Text("For admins"))
                                .onSubmit { model.start(connect: true) }
                        }
                        .frame(maxWidth: .infinity)
                    }
                }
                // (each half the width, under the fields' columns)
                GridRow(alignment: .top) {
                    ServerList()
                    DisplaySection()
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            .labelsHidden()
            .textFieldStyle(.roundedBorder)
            .controlSize(.large)
        }
        .alert("Rename Server", isPresented: Binding(get: { model.renaming != nil },
                                                     set: { if !$0 { model.renaming = nil } })) {
            TextField("Name", text: $model.renameText)
            Button("Rename") { model.commitRename() }
            Button("Cancel", role: .cancel) {}
        }
        .alert("Remove Server?", isPresented: Binding(get: { model.removing != nil },
                                                      set: { if !$0 { model.removing = nil } })) {
            Button("Remove", role: .destructive) { model.commitRemove() }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("\(model.removingLabel) and its passwords leave the list.")
        }
    }
}

/// The built-in servers and the bookmarked ones, listRows at most: all in view, nothing to scroll
struct ServerList: View {
    @EnvironmentObject var model: LauncherModel

    var body: some View {
        let listed = model.listed
        VStack(spacing: 0) {
            ForEach(listed) { item in
                ServerRow(item: item)
            }
        }
        .padding(2)
        .frame(maxWidth: .infinity)
        .frame(height: CGFloat(listRows) * Metrics.row + 4, alignment: .top)
        .background(RoundedRectangle(cornerRadius: 6).fill(Color.black.opacity(0.19)))
        .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(Color.white.opacity(0.04)))
        .overlay(alignment: .bottom) {
            // the room the list doesn't fill yet
            if listed.count < listRows {
                Text(listed.isEmpty ? "Bookmark a server to keep it here: double-click one to join it."
                     : "Bookmark a server to keep it here.")
                    .font(.system(size: 12))
                    .foregroundStyle(.tertiary)
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, Metrics.gap)
                    .frame(maxWidth: .infinity)
                    .frame(height: CGFloat(listRows - listed.count) * Metrics.row)
                    .allowsHitTesting(false)
            }
        }
    }
}

struct ServerRow: View {
    @EnvironmentObject var model: LauncherModel
    let item: Listed
    @State var hover = false

    var body: some View {
        let selected = model.isSelected(item)
        HStack(spacing: 2) {
            // the name, and under it the address when the name isn't the address
            VStack(alignment: .leading, spacing: 0) {
                Text(item.server.label)
                    .fontWeight(item.builtin ? .semibold : .regular)
                    .lineLimit(1)
                if item.server.label != item.server.address {
                    Text(item.server.address)
                        .font(.system(size: 11))
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .help(item.server.address)
            if item.builtin {
                // the same columns as the other rows, and a pin for a server that stays
                Color.clear.frame(width: 28, height: 28)
                Image(systemName: "pin.fill")
                    .font(.system(size: 11))
                    .foregroundStyle(.tertiary)
                    .frame(width: 28, height: 28)
                    .help("Always in the list")
            } else {
                RowButton(systemImage: "pencil", help: "Rename") { model.beginRename(item) }
                RowButton(systemImage: "trash", help: "Remove") { model.beginRemove(item) }
            }
            RowButton(systemImage: "play.fill", help: "Join", accent: true, enabled: model.canStart) {
                model.connect(item)
            }
        }
        .padding(.leading, Metrics.gap)
        .padding(.trailing, 3)
        .frame(height: Metrics.row)
        .background(RoundedRectangle(cornerRadius: 4)
            .fill(selected ? Color.selectedFill : hover ? Color.hoverFill : Color.clear))
        .contentShape(Rectangle())
        .onTapGesture(count: 2) { model.connect(item) }
        .onTapGesture { model.fill(item) }
        .onHover { hover = $0 }
        .contextMenu {
            Button("Join") { model.connect(item) }
            if !item.builtin {
                Button("Rename…") { model.beginRename(item) }
                Button("Remove", role: .destructive) { model.beginRemove(item) }
            }
        }
    }
}

struct RowButton: View {
    let systemImage: String
    let help: String
    var accent = false
    var enabled = true
    @State var hover = false
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: systemImage)
                .font(.system(size: 12))
                .foregroundStyle(!enabled ? Color.secondary.opacity(0.4)
                    : accent ? Color.accentColor : Color.primary.opacity(0.85))
                .frame(width: 28, height: 28)
                .background(RoundedRectangle(cornerRadius: 4).fill(hover && enabled ? Color.selectedFill : Color.clear))
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .onHover { hover = $0 }
        .help(help)
    }
}

/// r_mode with r_customwidth and r_customheight, r_fullscreenStretch; r_fullscreen, r_noborder and r_borderlessWindow
struct DisplaySection: View {
    @EnvironmentObject var model: LauncherModel

    var body: some View {
        let mode = model.displayMode.wrappedValue
        VStack(alignment: .leading, spacing: Metrics.gap) {
            Field("Resolution", accessory: {
                Picker("Scaling", selection: model.scaling) {
                    ForEach(FullscreenScaling.allCases) { scaling in
                        Text(scaling.title).tag(scaling)
                    }
                }
                .buttonStyle(.borderless)
                .controlSize(.small)
                .fixedSize() // the name whole
                .disabled(!model.scalingApplies)
                .help(model.scalingApplies
                      ? "How this size fits the screen. Fit: its own shape, as large as it fits. Stretch: fills the screen. 1:1: pixel for pixel, centered between black bars."
                      : mode == .exclusive ? "This is the screen's own size: there's nothing to scale"
                      : "Only exclusive fullscreen scales the game to the screen")
            }) {
                Picker("Resolution", selection: model.resolution) {
                    ForEach(model.resolutionEntries) { entry in
                        Text(entry.label).tag(entry)
                    }
                }
                .disabled(!mode.usesResolution)
                .help(mode.usesResolution ? "The game's size" : "\(mode.title) takes the desktop's resolution")
            }
            Field("Display mode") {
                Picker("Display mode", selection: model.displayMode) {
                    ForEach(DisplayMode.allCases) { mode in
                        Text(mode.title).tag(mode)
                    }
                }
            }
        }
        .labelsHidden()
        .controlSize(.large)
    }
}

/// The project's links, where the game is (a click changes it), and the buttons
struct Footer: View {
    @EnvironmentObject var model: LauncherModel

    var body: some View {
        HStack(spacing: Metrics.gap / 2) {
            LinkButton(image: model.logo("github"), help: "MoHAA Reloaded on GitHub", hoverColor: .primary) {
                NSWorkspace.shared.open(LauncherModel.githubURL)
            }
            LinkButton(image: model.logo("discord"), help: "Forte's Discord", hoverColor: .discord) {
                NSWorkspace.shared.open(LauncherModel.discordURL)
            }
            Button {
                model.chooseGameFolder()
            } label: {
                HStack(spacing: 6) {
                    if model.running {
                        Circle().fill(Color.green).frame(width: 8, height: 8).frame(width: 14)
                    } else {
                        Image(systemName: "folder.fill").font(.system(size: 11))
                    }
                    Text(model.status)
                        .font(.system(size: 12))
                        .lineLimit(1)
                        .truncationMode(.tail)
                }
                .foregroundStyle(model.problem ? Color.warning : Color.secondary)
                // the links' icons sit inset in their boxes: as far from the Discord icon as it is from GitHub's
                .padding(.leading, LinkButton.inset)
                .frame(maxWidth: .infinity, alignment: .leading)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .help("The game folder: click to choose another")
            LabelAction(systemImage: model.closeOnStart ? "checkmark.square.fill" : "square",
                        title: "Close on start", active: model.closeOnStart) {
                model.closeOnStart.toggle()
            }
            .help("Quit MoHAA Launcher once the game has started")
            .padding(.horizontal, Metrics.gap / 2)
            Button {
                model.start(connect: false)
            } label: {
                Label("Single Player", systemImage: "person.fill")
            }
            .controlSize(.large)
            .disabled(!model.canStart)
            .help("Start the game at its main menu, without joining a server")
            Button {
                model.start(connect: true)
            } label: {
                Label("Connect", systemImage: "play.fill").frame(minWidth: 90)
            }
            .controlSize(.large)
            .buttonStyle(.borderedProminent)
            .keyboardShortcut(.defaultAction)
            .disabled(!model.canConnect)
            .help("Start the game and join the server")
        }
        .alert("MoHAA Launcher", isPresented: Binding(get: { model.alertMessage != nil },
                                                      set: { if !$0 { model.alertMessage = nil } })) {
            Button("OK") {}
        } message: {
            Text(model.alertMessage ?? "")
        }
    }
}

/// An icon that opens a web page
struct LinkButton: View {
    static let icon: CGFloat = 17
    static let box: CGFloat = 28 // the hover background and the click target
    static let inset = (box - icon) / 2
    let image: NSImage
    let help: String
    let hoverColor: Color
    @State var hover = false
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(nsImage: image)
                .renderingMode(.template)
                .resizable()
                .frame(width: LinkButton.icon, height: LinkButton.icon)
                .foregroundStyle(hover ? hoverColor : Color.secondary)
                .frame(width: LinkButton.box, height: LinkButton.box)
                .background(RoundedRectangle(cornerRadius: 4).fill(hover ? Color.hoverFill : Color.clear))
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hover = $0 }
        .help(help)
    }
}
