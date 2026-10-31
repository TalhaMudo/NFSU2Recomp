// The launcher window.  SwiftUI as available on macOS 11 (the app's minimum).
import SwiftUI

enum Theme {
    static let background = Color(red: 0.067, green: 0.071, blue: 0.082)
    static let card = Color.white.opacity(0.045)
    static let cardBorder = Color.white.opacity(0.08)
    static let accent = Color(red: 0.62, green: 0.87, blue: 0.27)      // the game's menu green
    static let warn = Color(red: 1.0, green: 0.68, blue: 0.24)
    static let bad = Color(red: 1.0, green: 0.38, blue: 0.36)
    static let text = Color.white.opacity(0.92)
    static let dim = Color.white.opacity(0.55)
}

struct PrimaryButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View { StyledLabel(configuration: configuration) }
    private struct StyledLabel: View {
        let configuration: Configuration
        @Environment(\.isEnabled) var enabled
        var body: some View {
            configuration.label
                .font(.system(size: 15, weight: .bold))
                .foregroundColor(Color.black.opacity(enabled ? 0.88 : 0.45))
                .padding(.horizontal, 30).padding(.vertical, 9)
                .background(RoundedRectangle(cornerRadius: 8)
                    .fill(enabled ? Theme.accent.opacity(configuration.isPressed ? 0.72 : 1)
                                  : Color.white.opacity(0.15)))
        }
    }
}

struct SecondaryButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View { StyledLabel(configuration: configuration) }
    private struct StyledLabel: View {
        let configuration: Configuration
        @Environment(\.isEnabled) var enabled
        var body: some View {
            configuration.label
                .font(.system(size: 12, weight: .medium))
                .foregroundColor(Theme.text.opacity(enabled ? 1 : 0.4))
                .padding(.horizontal, 11).padding(.vertical, 6)
                .background(RoundedRectangle(cornerRadius: 6)
                    .fill(Color.white.opacity(configuration.isPressed ? 0.16 : 0.09)))
        }
    }
}

struct Card<Content: View>: View {
    let title: String
    let content: Content
    init(_ title: String, @ViewBuilder content: () -> Content) { self.title = title; self.content = content() }
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(title.uppercased())
                .font(.system(size: 10.5, weight: .semibold)).foregroundColor(Theme.dim)
                .tracking(0.8)
            content
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 10).fill(Theme.card))
        .overlay(RoundedRectangle(cornerRadius: 10).stroke(Theme.cardBorder, lineWidth: 1))
    }
}

struct GameCard: View {
    @ObservedObject var model: LauncherModel

    private var status: (Color, String, String) {
        switch model.game {
        case .searching:
            return (Theme.dim, "Looking for the game…", "")
        case .notFound:
            return (Theme.warn, "Game files not found",
                    "Choose the folder where Need for Speed Underground 2 is installed (the folder with SPEED2.EXE in it). Your own copy of the PC game is needed.")
        case .ready(let l):
            return (Theme.accent, l.embedded ? "Ready · the game is inside this app" : "Ready · \(l.build?.name ?? "supported version")",
                    l.embedded ? "This is a portable copy: it needs no separate game folder." : l.folder.path)
        case .unsupported(let l, let size):
            if let b = l.build {
                return (Theme.bad, "No engine for this version in this app",
                        "\(l.folder.path) has the \(b.name) SPEED2.EXE, but this copy of the app has no engine for it. Use a release that includes it.")
            }
            return (Theme.bad, "This SPEED2.EXE is a different version",
                    "Found \(formatBytes(Int64(size))) at \(l.folder.path). The port runs the European v1.2 (4,788,224 bytes) and North American v1.2 (4,800,512 bytes) SPEED2.EXE without the CD check; other versions, such as the US disc one, would crash.")
        }
    }

    var body: some View {
        Card("Game files") {
            HStack(alignment: .top, spacing: 10) {
                Circle().fill(status.0).frame(width: 9, height: 9).padding(.top, 4)
                VStack(alignment: .leading, spacing: 3) {
                    Text(status.1).font(.system(size: 13, weight: .semibold)).foregroundColor(Theme.text)
                    if !status.2.isEmpty {
                        Text(status.2).font(.system(size: 11)).foregroundColor(Theme.dim)
                            .lineLimit(3).truncationMode(.middle)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                Spacer(minLength: 8)
            }
            HStack(spacing: 8) {
                if model.location?.embedded != true {
                    Button(model.location == nil ? "Choose Folder…" : "Change Folder…") { model.chooseFolder() }
                        .buttonStyle(SecondaryButtonStyle())
                }
                if model.location != nil {
                    Button("Show in Finder") { model.revealGame() }.buttonStyle(SecondaryButtonStyle())
                }
            }
        }
    }
}

struct GraphicsCard: View {
    @ObservedObject var model: LauncherModel
    var body: some View {
        Card("Graphics") {
            if model.canChooseEngine {
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
            HStack(alignment: .center) {
                VStack(alignment: .leading, spacing: 1) {
                    Text("Start in full screen").font(.system(size: 12.5)).foregroundColor(Theme.text)
                    Text("Cmd-F switches while playing. Resolution: in the game, Options › Display.")
                        .font(.system(size: 11)).foregroundColor(Theme.dim)
                }
                Spacer(minLength: 8)
                Toggle("", isOn: $model.fullscreen).labelsHidden().toggleStyle(SwitchToggleStyle())
            }
            HStack(alignment: .center) {
                VStack(alignment: .leading, spacing: 1) {
                    Text("HUD at the screen edges").font(.system(size: 12.5)).foregroundColor(Theme.text)
                    Text("On wide screens the minimap moves to the left and the gauges to the right. Off: the HUD stays in the 4:3 middle.")
                        .font(.system(size: 11)).foregroundColor(Theme.dim)
                        .fixedSize(horizontal: false, vertical: true)
                }
                Spacer(minLength: 8)
                Toggle("", isOn: $model.wideHUD).labelsHidden().toggleStyle(SwitchToggleStyle())
            }
        }
    }
}

struct ControllerCard: View {
    @ObservedObject var model: LauncherModel
    var body: some View {
        Card("Controller") {
            HStack(alignment: .center) {
                VStack(alignment: .leading, spacing: 1) {
                    Text("Use a game controller").font(.system(size: 12.5)).foregroundColor(Theme.text)
                    Text("Xbox, PlayStation or MFi. Connect it before Play; buttons are set in the game's Controls menu.")
                        .font(.system(size: 11)).foregroundColor(Theme.dim)
                        .fixedSize(horizontal: false, vertical: true)
                }
                Spacer(minLength: 8)
                Toggle("", isOn: $model.controller).labelsHidden().toggleStyle(SwitchToggleStyle())
            }
            HStack(alignment: .center, spacing: 10) {
                Text("Stick dead zone").font(.system(size: 12.5)).foregroundColor(Theme.text)
                Slider(value: $model.stickDeadzone, in: 0...0.5, step: 0.01)
                Text("\(Int((model.stickDeadzone * 100).rounded())) %")
                    .font(.system(size: 11.5).monospacedDigit()).foregroundColor(Theme.dim)
                    .frame(width: 40, alignment: .trailing)
            }
            .disabled(!model.controller).opacity(model.controller ? 1 : 0.5)
        }
    }
}

struct PictureCard: View {
    @ObservedObject var model: LauncherModel
    private func row(_ i: Int) -> some View {
        let o = PictureOptions.all[i]
        return VStack(alignment: .leading, spacing: 3) {
            Text(o.title).font(.system(size: 12)).foregroundColor(Theme.text)
            Picker("", selection: $model.picture[i]) {
                ForEach(0..<o.choices.count, id: \.self) { k in Text(o.choices[k].label).tag(k) }
            }
            .pickerStyle(MenuPickerStyle())
            .labelsHidden()
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
    var body: some View {
        Card("Picture") {
            let n = PictureOptions.all.count
            VStack(alignment: .leading, spacing: 8) {
                ForEach(0..<(n + 1) / 2, id: \.self) { r in
                    HStack(spacing: 18) {
                        row(2 * r)
                        if 2 * r + 1 < n { row(2 * r + 1) } else { Spacer().frame(maxWidth: .infinity) }
                    }
                }
            }
            Text("(original) is the game as it was on PC.")
                .font(.system(size: 11)).foregroundColor(Theme.dim)
        }
    }
}

struct MoreCard: View {
    @ObservedObject var model: LauncherModel
    var body: some View {
        Card("More") {
            HStack(alignment: .center, spacing: 12) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Portable copy").font(.system(size: 12.5, weight: .medium)).foregroundColor(Theme.text)
                    Text("One app with the game inside, for another Mac or a USB drive. Movies and language packs can be left out to make it smaller.")
                        .font(.system(size: 11)).foregroundColor(Theme.dim)
                        .fixedSize(horizontal: false, vertical: true)
                }
                Spacer(minLength: 6)
                Button("Make Copy…") { model.makePortable() }
                    .buttonStyle(SecondaryButtonStyle())
                    .disabled(!model.isReady)
            }
            HStack(spacing: 8) {
                Button("Saves…") { model.manageSaves() }.buttonStyle(SecondaryButtonStyle())
                Button("Logs") { model.openLogs() }.buttonStyle(SecondaryButtonStyle())
                Button("Reset Graphics Settings…") { model.resetGraphics() }.buttonStyle(SecondaryButtonStyle())
                Spacer(minLength: 0)
                Button("Report a Problem") { model.reportProblem() }.buttonStyle(SecondaryButtonStyle())
            }
            if let n = model.notice {
                Text(n).font(.system(size: 11)).foregroundColor(Theme.accent)
            }
        }
    }
}

struct JobOverlay: View {
    @ObservedObject var model: LauncherModel
    let job: LauncherModel.Job
    var body: some View {
        ZStack {
            Color.black.opacity(0.6)
            VStack(alignment: .leading, spacing: 12) {
                if let r = job.result {
                    Text("Portable copy ready").font(.system(size: 15, weight: .bold)).foregroundColor(Theme.text)
                    Text("“\(r.deletingPathExtension().lastPathComponent)” has the game inside and runs on its own; you can copy it to another Mac. Saves stay on each Mac, in Application Support › NFSU2 Saves.")
                        .font(.system(size: 12)).foregroundColor(Theme.dim)
                        .fixedSize(horizontal: false, vertical: true)
                    HStack {
                        Spacer()
                        Button("Close") { model.closeJob() }.buttonStyle(SecondaryButtonStyle())
                        Button("Show in Finder") { model.revealResult() }.buttonStyle(PrimaryButtonStyle())
                    }
                } else if let e = job.error {
                    Text("The copy could not be made").font(.system(size: 15, weight: .bold)).foregroundColor(Theme.text)
                    Text(e).font(.system(size: 12)).foregroundColor(Theme.dim)
                        .fixedSize(horizontal: false, vertical: true)
                    HStack { Spacer(); Button("Close") { model.closeJob() }.buttonStyle(SecondaryButtonStyle()) }
                } else {
                    Text("Making a portable copy").font(.system(size: 15, weight: .bold)).foregroundColor(Theme.text)
                    Text(job.phase).font(.system(size: 12)).foregroundColor(Theme.dim)
                    ProgressView(value: job.fraction).accentColor(Theme.accent)
                    HStack {
                        Text(job.total > 0 ? "\(formatBytes(job.done)) of \(formatBytes(job.total))" : " ")
                            .font(.system(size: 11)).foregroundColor(Theme.dim)
                        Spacer()
                        Button("Cancel") { model.cancelPortable() }.buttonStyle(SecondaryButtonStyle())
                    }
                }
            }
            .padding(20)
            .frame(width: 380)
            .background(RoundedRectangle(cornerRadius: 12).fill(Color(red: 0.11, green: 0.115, blue: 0.13)))
            .overlay(RoundedRectangle(cornerRadius: 12).stroke(Theme.cardBorder, lineWidth: 1))
        }
    }
}

struct LauncherView: View {
    @ObservedObject var model: LauncherModel

    private var versionText: String {
        let info = Bundle.main.infoDictionary ?? [:]
        let v = (info["CFBundleShortVersionString"] as? String) ?? "?"
        let b = (info["CFBundleVersion"] as? String) ?? ""
        return "Native macOS port · version \(v)" + (b.isEmpty ? "" : " (\(b))")
    }

    var body: some View {
        ZStack {
            Theme.background
            VStack(alignment: .leading, spacing: 12) {
                HStack(spacing: 14) {
                    Image(nsImage: NSApp.applicationIconImage)
                        .resizable().frame(width: 64, height: 64)
                    VStack(alignment: .leading, spacing: 3) {
                        Text("Need for Speed Underground 2")
                            .font(.system(size: 21, weight: .bold)).foregroundColor(Theme.text)
                        Text(versionText).font(.system(size: 11.5)).foregroundColor(Theme.dim)
                    }
                }
                .padding(.bottom, 4)

                GameCard(model: model)
                GraphicsCard(model: model)
                ControllerCard(model: model)
                PictureCard(model: model)
                MoreCard(model: model)

                Spacer(minLength: 0)
                HStack(alignment: .center) {
                    VStack(alignment: .leading, spacing: 2) {
                        Toggle("Show this window when the app opens", isOn: $model.showAtStartup)
                            .toggleStyle(CheckboxToggleStyle())
                            .font(.system(size: 12)).foregroundColor(Theme.text)
                        Text("When off, hold ⌥ Option while opening the app to see it again.")
                            .font(.system(size: 10.5)).foregroundColor(Theme.dim)
                    }
                    Spacer()
                    Button("Play") { model.play() }
                        .buttonStyle(PrimaryButtonStyle())
                        .keyboardShortcut(.defaultAction)
                        .disabled(!model.isReady || model.job != nil)
                }
            }
            .padding(.horizontal, 22).padding(.top, 10).padding(.bottom, 18)
            if let j = model.job { JobOverlay(model: model, job: j) }
        }
        .frame(width: 560, height: 884)
    }
}
