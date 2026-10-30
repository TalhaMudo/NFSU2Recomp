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
    static let shown: [SettingsTab] = [.graphics, .more]

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
    init(title: String, description: String = "", @ViewBuilder control: () -> Control) {
        self.title = title; self.description = description; self.control = control()
    }
    var body: some View {
        HStack(alignment: .center, spacing: 12) {
            VStack(alignment: .leading, spacing: 1) {
                Text(title).font(.system(size: 12.5)).foregroundColor(Theme.text)
                if !description.isEmpty {
                    Text(description).font(.system(size: 11)).foregroundColor(Theme.dim)
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
        }
    }
}

struct MoreSettings: View {
    @ObservedObject var model: LauncherModel
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Card {
                SettingRow(title: "Portable copy",
                           description: "One app with the game inside (about 1.4 GB), for another Mac or a USB drive.") {
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

struct SettingsView: View {
    static let size = CGSize(width: 560, height: 470)
    @ObservedObject var model: LauncherModel
    @State private var tab: SettingsTab

    init(model: LauncherModel, tab: SettingsTab = SettingsTab.shown[0]) {
        self.model = model
        _tab = State(initialValue: tab)
    }

    @ViewBuilder private var page: some View {
        switch tab {
        case .graphics: GraphicsSettings(model: model)
        case .more: MoreSettings(model: model)
        case .picture, .controls, .online, .game, .mods: EmptyView()      // no rows yet
        }
    }

    private var tabBar: some View {
        HStack(spacing: 4) {
            ForEach(SettingsTab.shown, id: \.self) { t in
                Button(action: { tab = t }) {
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
