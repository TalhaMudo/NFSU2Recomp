// The Settings window: the launcher's options, on tabs.  The main window
// (Views.swift) stays small and only has what a player needs before Play.
//
// Adding an option:
//   1. Game.swift: a Prefs key, and the value that gives the original game in
//      Prefs.originalGame (Restore Original Game walks that list).
//   2. Model.swift: a @Published property (read in init, saved in didSet) and its
//      setenv(..., 0) in play(), so a variable set by hand still wins.
//   3. Here: a SettingRow in its tab's view, inside a Card:
//          SettingRow(title: "Frame-rate counter",
//                     description: "Frames per second in a corner. Off: as in the original game.",
//                     isOn: $model.fpsCounter)
//      Other controls go in the closure: SettingRow(title: ..., description: ...) { Picker(...) }.
//      A switch's description ends with "Off: as in the original game."; a picker
//      labels the choice that gives the original game "(original)".
//      A Card holds any views, so it can also end with its own buttons:
//          Card("Data mods") { SettingRow(...) ...; HStack { Spacer(); Button("Add File…") { ... } } }
// A tab with no rows is hidden: list it in SettingsTab.shown and give it a view in
// SettingsView.page when its first row lands.
import AppKit
import SwiftUI

enum SettingsTab: String, CaseIterable {
    case graphics, picture, controls, online, game, mods, more

    /// The tabs that have rows, in this order.
    static let shown: [SettingsTab] = [.graphics, .picture, .controls, .mods, .more]

    var title: String { rawValue.capitalized }
    var icon: String {          // SF Symbols available on macOS 11
        switch self {
        case .graphics: return "display"
        case .picture: return "camera.filters"
        case .controls: return "gamecontroller"
        case .online: return "network"
        case .game: return "car"
        case .mods: return "puzzlepiece"
        case .more: return "ellipsis.circle"
        }
    }
}

/// One option: title, dim description, the control on the right.
struct SettingRow<Control: View>: View {
    let title: String
    let description: String
    let control: Control
    var descriptionColor = Theme.dim       // Theme.warn for a message the player should read
    init(title: String, description: String = "", descriptionColor: Color = Theme.dim,
         @ViewBuilder control: () -> Control) {
        self.title = title; self.description = description; self.descriptionColor = descriptionColor
        self.control = control()
    }
    var body: some View {
        HStack(alignment: .center, spacing: 12) {
            VStack(alignment: .leading, spacing: 1) {
                Text(title).font(.system(size: 12.5)).foregroundColor(Theme.text)
                if !description.isEmpty {
                    Text(description).font(.system(size: 11)).foregroundColor(descriptionColor)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            Spacer(minLength: 8)
            control
        }
    }
}

struct SettingSwitch: View {
    @Binding var isOn: Bool
    var body: some View { Toggle("", isOn: $isOn).labelsHidden().toggleStyle(SwitchToggleStyle()) }
}

extension SettingRow where Control == SettingSwitch {
    /// A switch.
    init(title: String, description: String = "", isOn: Binding<Bool>) {
        self.init(title: title, description: description) { SettingSwitch(isOn: isOn) }
    }
}

// MARK: tabs

struct GraphicsSettings: View {
    @ObservedObject var model: LauncherModel
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if model.canChooseEngine {
                Card("Engine") {
                    Picker("", selection: $model.engine) {
                        Text("Original").tag(LauncherModel.Engine.original)
                        Text("HD").tag(LauncherModel.Engine.hd)
                    }
                    .pickerStyle(SegmentedPickerStyle())
                    .labelsHidden()
                    .fixedSize()
                    Text(model.engine == .hd
                         ? "Anti-aliasing and ray-traced reflections on top of the game's own look. The Mac works harder and gets warmer."
                         : "The game exactly as it looked on PC. Uses the least power.")
                        .font(.system(size: 11)).foregroundColor(Theme.dim)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            Card("HUD") {
                SettingRow(title: "HUD at the screen edges",
                           description: "On wide screens the minimap moves to the left and the gauges to the right. Off: the HUD stays in the 4:3 middle.",
                           isOn: $model.wideHUD)
            }
            Card("Frame rate") {
                SettingRow(title: "Frames a second",
                           description: "The game's own limit is 120. 60 uses less power and suits 60 Hz screens; the screen's refresh rate is the upper limit. The game runs at the same speed either way.") {
                    Picker("", selection: $model.frameRate) {
                        Text("30").tag(30); Text("60").tag(60); Text("120").tag(120); Text("240").tag(240)
                    }
                    .pickerStyle(SegmentedPickerStyle()).labelsHidden().fixedSize()
                }
                SettingRow(title: "Show the frame rate",
                           description: "Frames a second in the top left corner while playing.",
                           isOn: $model.fpsCounter)
            }
        }
    }
}

/// HD engine only: the original engine is the game's 2004 picture.
struct PictureSettings: View {
    @ObservedObject var model: LauncherModel
    private var active: Bool { model.engine == .hd && model.hasEngine(.hd) }
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Card("Picture") {
                Text(!model.hasEngine(.hd)
                     ? "These need the HD engine, which is not in this app."
                     : active ? "HD engine only. (original) is the game as it was on PC."
                              : "HD engine only: choose HD in Graphics to use them. The Original engine always shows the game as it was on PC.")
                    .font(.system(size: 11)).foregroundColor(active ? Theme.dim : Theme.accent)
                    .fixedSize(horizontal: false, vertical: true)
                ForEach(0..<PictureOptions.all.count, id: \.self) { i in
                    let o = PictureOptions.all[i]
                    SettingRow(title: o.title, description: o.description) {
                        Picker("", selection: $model.picture[i]) {
                            ForEach(0..<o.choices.count, id: \.self) { k in Text(o.choices[k].label).tag(k) }
                        }
                        .pickerStyle(MenuPickerStyle()).labelsHidden().fixedSize()
                    }
                    .disabled(!active).opacity(active ? 1 : 0.5)
                }
            }
            Card("Reflections") {
                Group {
                    SettingRow(title: "Sky in reflections",
                               description: "The wet road and the rear-view mirror show the sky instead of black. Off: as in the original game.",
                               isOn: $model.reflectSky)
                    SettingRow(title: "Car lights in reflections",
                               description: "Head and tail lights of the cars shine in the wet road. Off: as in the original game.",
                               isOn: $model.reflectLights)
                    SettingRow(title: "More detail in reflections",
                               description: "Full-detail buildings and cars in the reflections, and the car paint reflects the city. More work for the Mac. Off: as in the original game.",
                               isOn: $model.reflectDetail)
                }
                .disabled(!active).opacity(active ? 1 : 0.5)
            }
        }
    }
}

struct ControlsSettings: View {
    @ObservedObject var model: LauncherModel
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Card("Controller") {
                SettingRow(title: "Use a game controller",
                           description: "Xbox, PlayStation or MFi. Connect it before Play; buttons are set in the game's Controls menu.",
                           isOn: $model.controller)
                SettingRow(title: "Stick dead zone",
                           description: "How far the stick moves before the game reacts. Raise it for a stick that drifts.") {
                    HStack(spacing: 8) {
                        Slider(value: $model.stickDeadzone, in: 0...0.5, step: 0.01).frame(width: 140)
                        Text("\(Int((model.stickDeadzone * 100).rounded())) %")
                            .font(.system(size: 11.5).monospacedDigit()).foregroundColor(Theme.dim)
                            .frame(width: 40, alignment: .trailing)
                    }
                }
                .disabled(!model.controller).opacity(model.controller ? 1 : 0.5)
                SettingRow(title: "XtendedInput layout",
                           description: model.canUseXtendedInput
                               ? "RT accelerates, LT brakes, every button as in the NFSU-XtendedInput mod, set in scripts/NFSU_XtendedInput.ini in the game folder if there is one. Off: buttons are set in the game's Controls menu."
                               : "Needs the North American v1.2 SPEED2.EXE.",
                           isOn: $model.xtendedInput)
                .disabled(!model.controller || !model.canUseXtendedInput)
                .opacity(model.controller && model.canUseXtendedInput ? 1 : 0.5)
            }
        }
    }
}

struct ModsSettings: View {
    @ObservedObject var model: LauncherModel
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Card("NFSU2 Unlimiter") { UnlimiterRow(model: model) }
            Card("Mods") {
                ForEach(model.otherMods) { m in ModRow(model: model, mod: m) }
                if model.otherMods.isEmpty {
                    Text("No mods yet. Mods from nfsmods.xyz and similar sites: ones with an install script for Binary or Binarius (install.end, .endlauncher), and ones that are plain game files (folders such as TRACKS, GLOBAL, CARS).")
                        .font(.system(size: 11)).foregroundColor(Theme.dim)
                        .fixedSize(horizontal: false, vertical: true)
                }
                HStack(alignment: .center, spacing: 8) {
                    if let n = model.modNote["add"] {
                        Text(n).font(.system(size: 11))
                            .foregroundColor(model.modBusy.contains("add") ? Theme.dim : Theme.warn)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    Spacer(minLength: 8)
                    Button("Add Mod…") { model.addMod() }
                        .buttonStyle(SecondaryButtonStyle())
                        .disabled(model.modBusy.contains("add") || !model.isReady)
                }
            }
            if let s = model.modsApplying {
                Text(s).font(.system(size: 11)).foregroundColor(Theme.accent).padding(.horizontal, 4)
            }
            Text("The game folder is never changed: the mods are installed into a copy, and only the files they change are kept, in Application Support. Off: as in the original game. Mods with a plugin (.asi) other than Unlimiter need it translated for the Mac first and are not taken.")
                .font(.system(size: 11)).foregroundColor(Theme.dim)
                .fixedSize(horizontal: false, vertical: true)
                .padding(.horizontal, 4)
        }
    }
}

struct UnlimiterRow: View {
    @ObservedObject var model: LauncherModel
    var body: some View {
        let id = Mods.Unlimiter.id
        let m = model.unlimiter, problem = model.unlimiterProblem
        let busy = model.modBusy.contains(id), note = model.modNote[id]
        VStack(alignment: .leading, spacing: 8) {
            SettingRow(title: "\(Mods.Unlimiter.name) \(Mods.Unlimiter.version)  ·  \(Mods.Unlimiter.author)",
                       description: note ?? problem ?? (m != nil
                            ? "More cars, parts, rims and paints for add-on mods; separate front and rear rims, more rim brands. Off: as in the original game."
                            : "More cars, parts, rims and paints for add-on mods. \(formatBytes(Mods.Unlimiter.size)) from GitHub."),
                       descriptionColor: (note != nil && !busy) || problem != nil ? Theme.warn : Theme.dim) {
                if let m = m {
                    Toggle("", isOn: Binding(get: { m.on }, set: { model.setMod(m, $0) }))
                        .labelsHidden().toggleStyle(SwitchToggleStyle())
                        .disabled(busy || problem != nil || model.modsApplying != nil)
                } else {
                    HStack(spacing: 6) {
                        Button("Download") { model.downloadUnlimiter() }
                            .buttonStyle(SecondaryButtonStyle()).disabled(busy || problem != nil)
                        Button("Add File…") { model.addUnlimiterFile() }
                            .buttonStyle(SecondaryButtonStyle()).disabled(busy || problem != nil)
                    }
                }
            }
            if let m = m, m.on, problem == nil { ModQuestions(model: model, mod: m, hidden: Mods.Unlimiter.hidden) }
        }
        .contextMenu {
            Button("Open Unlimiter's Page") { NSWorkspace.shared.open(Mods.Unlimiter.page) }
            if let m = m {
                Button("Show in Finder") { model.revealMod(m) }
                Button("Remove \(Mods.Unlimiter.name)") { model.removeMod(m) }
            }
        }
    }
}

/// One added mod: its switch, what it is, its questions.
struct ModRow: View {
    @ObservedObject var model: LauncherModel
    let mod: InstalledMod
    var body: some View {
        let note = model.modNote[mod.id]
        let what = mod.kind == .script ? "Binary / Binarius script" : "game files"
        let issues = mod.issues.isEmpty ? "" : " The last install reported \(mod.issues.count == 1 ? "a problem" : "\(mod.issues.count) problems"): \(mod.issues[0].message ?? "")"
        VStack(alignment: .leading, spacing: 8) {
            SettingRow(title: mod.name,
                       description: note ?? "\(mod.source), \(what).\(issues)",
                       descriptionColor: note != nil || !mod.issues.isEmpty ? Theme.warn : Theme.dim) {
                Toggle("", isOn: Binding(get: { mod.on }, set: { model.setMod(mod, $0) }))
                    .labelsHidden().toggleStyle(SwitchToggleStyle())
                    .disabled(model.modsApplying != nil)
            }
            if mod.on { ModQuestions(model: model, mod: mod, hidden: []) }
        }
        .contextMenu {
            Button("Show in Finder") { model.revealMod(mod) }
            Button("Remove \(mod.name)") { model.removeMod(mod) }
        }
    }
}

/// The questions a mod's script asked when it was installed, with the answers used;
/// another answer installs it again.
struct ModQuestions: View {
    @ObservedObject var model: LauncherModel
    let mod: InstalledMod
    let hidden: [String]
    var body: some View {
        let qs = mod.prompts.filter { q in !hidden.contains { q.description.contains($0) } }
        if !qs.isEmpty {
            VStack(alignment: .leading, spacing: 6) {
                ForEach(qs, id: \.description) { q in
                    HStack(alignment: .center, spacing: 8) {
                        // "NFSU2 Unlimiter: Please choose ..." under Unlimiter's own row: without the name
                        Text(q.description.hasPrefix(mod.name + ": ") ? String(q.description.dropFirst(mod.name.count + 2)) : q.description)
                            .font(.system(size: 11)).foregroundColor(Theme.dim)
                            .fixedSize(horizontal: false, vertical: true)
                        Spacer(minLength: 8)
                        Picker("", selection: Binding(get: { q.choice }, set: { model.setChoice(mod, q, $0) })) {
                            ForEach(q.options, id: \.self) { o in Text(o).tag(o) }
                        }
                        .labelsHidden().pickerStyle(MenuPickerStyle()).frame(maxWidth: 210)
                        .disabled(model.modsApplying != nil)
                    }
                }
            }
            .padding(.leading, 14)
        }
    }
}

struct MoreSettings: View {
    @ObservedObject var model: LauncherModel
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Card {
                SettingRow(title: "Portable copy",
                           description: "One app with the game inside, for another Mac or a USB drive. Movies and language packs can be left out to make it smaller.") {
                    Button("Make Copy…") { model.makePortable() }
                        .buttonStyle(SecondaryButtonStyle())
                        .disabled(!model.isReady)
                }
                SettingRow(title: "Graphics settings in the game",
                           description: "The game picks them for this Mac again on its next start, as on its first one. Career saves are not affected.") {
                    Button("Reset…") { model.resetGraphics() }.buttonStyle(SecondaryButtonStyle())
                }
            }
        }
    }
}

// MARK: window

/// The selected tab.  An ObservableObject rather than @State: with the command line
/// tools' Swift 6.4 compiler @State is a macro whose plugin isn't there (pack.sh).
final class SettingsSelection: ObservableObject {
    @Published var tab: SettingsTab
    init(_ tab: SettingsTab) { self.tab = tab }
}

struct SettingsView: View {
    static let size = CGSize(width: 560, height: 470)
    @ObservedObject var model: LauncherModel
    @ObservedObject private var selection: SettingsSelection
    private var tab: SettingsTab { selection.tab }

    init(model: LauncherModel, tab: SettingsTab = SettingsTab.shown[0]) {
        self.model = model
        self.selection = SettingsSelection(tab)
    }

    @ViewBuilder private var page: some View {
        switch tab {
        case .graphics: GraphicsSettings(model: model)
        case .picture: PictureSettings(model: model)
        case .controls: ControlsSettings(model: model)
        case .mods: ModsSettings(model: model)
        case .more: MoreSettings(model: model)
        case .online, .game: EmptyView()      // no rows yet
        }
    }

    private var tabBar: some View {
        HStack(spacing: 4) {
            ForEach(SettingsTab.shown, id: \.self) { t in
                Button(action: { selection.tab = t }) {
                    VStack(spacing: 3) {
                        Image(systemName: t.icon).font(.system(size: 17))
                            .frame(height: 22)
                        Text(t.title).font(.system(size: 11, weight: .medium))
                    }
                    .foregroundColor(t == tab ? Theme.accent : Theme.dim)
                    .frame(minWidth: 66).padding(.vertical, 6).padding(.horizontal, 4)
                    .background(RoundedRectangle(cornerRadius: 7)
                        .fill(Color.white.opacity(t == tab ? 0.08 : 0)))
                    .contentShape(Rectangle())
                }
                .buttonStyle(PlainButtonStyle())
            }
        }
    }

    var body: some View {
        ZStack {
            Theme.background
            VStack(spacing: 0) {
                tabBar.padding(.top, 2).padding(.bottom, 10)
                Rectangle().fill(Theme.cardBorder).frame(height: 1)
                ScrollView {
                    page.padding(.horizontal, 22).padding(.vertical, 14)
                }
                Rectangle().fill(Theme.cardBorder).frame(height: 1)
                HStack(alignment: .center) {
                    if let n = model.notice {
                        Text(n).font(.system(size: 11)).foregroundColor(Theme.accent)
                    } else if model.isOriginalGame {
                        Text("Every option is as in the original game.")
                            .font(.system(size: 11)).foregroundColor(Theme.dim)
                    }
                    Spacer(minLength: 8)
                    Button("Restore Original Game…") { model.confirmRestoreOriginalGame() }
                        .buttonStyle(SecondaryButtonStyle())
                        .disabled(model.isOriginalGame)
                        .help("Every option back to the value that gives the original, unmodified game.")
                }
                .padding(.horizontal, 22).padding(.vertical, 12)
            }
            if let j = model.job { JobOverlay(model: model, job: j) }
        }
        .frame(width: SettingsView.size.width, height: SettingsView.size.height)
    }
}

/// The Settings window: same look as the main window, a fixed size; a tab
/// taller than the window scrolls.
func makeSettingsWindow(model: LauncherModel, tab: SettingsTab) -> NSWindow {
    let w = NSWindow(contentRect: NSRect(origin: .zero, size: SettingsView.size),
                     styleMask: [.titled, .closable, .miniaturizable],
                     backing: .buffered, defer: false)
    w.titlebarAppearsTransparent = true
    w.title = "Settings"
    w.isMovableByWindowBackground = true
    w.isReleasedWhenClosed = false
    w.appearance = NSAppearance(named: .darkAqua)
    w.backgroundColor = NSColor(calibratedRed: 0.067, green: 0.071, blue: 0.082, alpha: 1)
    w.contentView = NSHostingView(rootView: SettingsView(model: model, tab: tab))
    w.center()
    return w
}
