import AppKit
import Foundation
import SwiftUI

private let defaultRepoPath = "/Users/simon/Documents/workspace/repos/sticks3-codex-companion"

struct RateLimit: Decodable {
    let label: String?
    let remainingPercent: Int?
    let usedPercent: Int?
    let windowMins: Int?

    enum CodingKeys: String, CodingKey {
        case label
        case remainingPercent = "remaining_percent"
        case usedPercent = "used_percent"
        case windowMins = "window_mins"
    }
}

struct RateLimits: Decodable {
    let primary: RateLimit?
    let secondary: RateLimit?
}

struct CurrentStatus: Decodable {
    let speaker: String?
    let kind: String?
    let text: String?
}

struct BridgePayload: Decodable {
    let active: Bool?
    let bridgeState: String?
    let codexState: String?
    let detail: String?
    let deviceError: String?
    let deviceState: String?
    let error: String?
    let launchAgentInstalled: Bool?
    let launchAgentLoaded: Bool?
    let logFile: String?
    let menuMode: String?
    let mode: String?
    let pid: Int?
    let rateLimits: RateLimits?
    let rollout: String?
    let runtimeDir: String?
    let state: String?
    let status: CurrentStatus?
    let supervisorPid: Int?
    let supervisorState: String?
    let taskActive: Bool?
    let threadId: String?
    let threadName: String?
    let tokens: UInt64?
    let updatedAt: String?

    enum CodingKeys: String, CodingKey {
        case active
        case bridgeState = "bridge_state"
        case codexState = "codex_state"
        case detail
        case deviceError = "device_error"
        case deviceState = "device_state"
        case error
        case launchAgentInstalled = "launch_agent_installed"
        case launchAgentLoaded = "launch_agent_loaded"
        case logFile = "log_file"
        case menuMode = "menu_mode"
        case mode
        case pid
        case rateLimits = "rate_limits"
        case rollout
        case runtimeDir = "runtime_dir"
        case state
        case status
        case supervisorPid = "supervisor_pid"
        case supervisorState = "supervisor_state"
        case taskActive = "task_active"
        case threadId = "thread_id"
        case threadName = "thread_name"
        case tokens
        case updatedAt = "updated_at"
    }
}

@MainActor
final class BridgeModel: ObservableObject {
    @Published var payload: BridgePayload?
    @Published var lastRefresh: Date?
    @Published var commandRunning = false
    @Published var commandMessage = ""

    let repoURL: URL
    let scriptURL: URL
    let statusURL: URL
    let logURL: URL

    private var timer: Timer?

    init() {
        let repoPath = BridgeModel.resolveRepoPath()
        repoURL = URL(fileURLWithPath: repoPath, isDirectory: true)
        scriptURL = repoURL.appendingPathComponent("scripts/sticks3-macos-bridge")
        statusURL = repoURL.appendingPathComponent("runtime/bridge-status.json")
        logURL = repoURL.appendingPathComponent("runtime/bridge.log")
        refresh()
        runBridgeCommand("ensure", announce: false)
        timer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { [weak self] _ in
            Task { @MainActor in
                self?.refresh()
            }
        }
    }

    static func resolveRepoPath() -> String {
        if let value = ProcessInfo.processInfo.environment["STICKS3_REPO"], !value.isEmpty {
            return value
        }
        if let value = Bundle.main.object(forInfoDictionaryKey: "STICKS3RepoPath") as? String, !value.isEmpty {
            return value
        }
        return defaultRepoPath
    }

    var menuTitle: String {
        "\(codexLabel) · \(deviceLabel)"
    }

    var symbolName: String {
        switch codexState {
        case "work", "working":
            return "bolt.horizontal.circle.fill"
        case "err", "error":
            return "exclamationmark.triangle.fill"
        case "idle", "connected":
            return "checkmark.circle.fill"
        case "wait", "starting":
            return "clock.circle.fill"
        default:
            return "circle"
        }
    }

    var codexState: String {
        guard let payload else { return "wait" }
        if let state = payload.codexState ?? payload.menuMode ?? payload.mode {
            return state
        }
        if payload.error != nil || payload.status?.kind == "error" {
            return "err"
        }
        if payload.active == true || payload.taskActive == true || payload.status?.kind == "working" || payload.status?.kind == "started" {
            return "work"
        }
        if payload.threadId != nil {
            return "idle"
        }
        return "wait"
    }

    var deviceState: String {
        guard let payload else { return "off" }
        if payload.supervisorState == "stopped" || payload.state == "stopped" {
            return "off"
        }
        return payload.deviceState ?? payload.state ?? "unknown"
    }

    var codexLabel: String {
        switch codexState {
        case "work", "working":
            return "Codex Work"
        case "idle", "connected":
            return "Codex Idle"
        case "err", "error":
            return "Codex Err"
        case "wait", "starting":
            return "Codex Wait"
        default:
            return "Codex \(codexState.capitalized)"
        }
    }

    var deviceLabel: String {
        switch deviceState {
        case "connected":
            return "S3 BLE"
        case "scanning", "connecting", "starting":
            return "S3 Scan"
        case "error":
            return "S3 Err"
        case "stopped", "off":
            return "S3 Off"
        case "disconnected":
            return "S3 Off"
        default:
            return "S3 \(deviceState.capitalized)"
        }
    }

    var statusLine: String {
        if let speaker = payload?.status?.speaker, let text = payload?.status?.text, !text.isEmpty {
            return "\(speaker): \(text)"
        }
        if let detail = payload?.detail, !detail.isEmpty {
            return detail
        }
        return "No bridge status yet"
    }

    var tokensLabel: String {
        guard let tokens = payload?.tokens else { return "n/a" }
        return Self.compact(tokens)
    }

    func refresh() {
        do {
            let data = try Data(contentsOf: statusURL)
            payload = try JSONDecoder().decode(BridgePayload.self, from: data)
            lastRefresh = Date()
        } catch {
            payload = nil
            lastRefresh = Date()
        }
    }

    func runBridgeCommand(_ command: String, announce: Bool = true) {
        if announce {
            guard !commandRunning else { return }
            commandRunning = true
            commandMessage = "Running \(command)..."
        }

        let script = scriptURL
        let repo = repoURL
        DispatchQueue.global(qos: .userInitiated).async {
            let process = Process()
            process.executableURL = script
            process.arguments = [command]
            process.currentDirectoryURL = repo
            process.environment = [
                "PATH": "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin",
                "STICKS3_REPO": repo.path
            ]

            let output = Pipe()
            process.standardOutput = output
            process.standardError = output

            do {
                try process.run()
                process.waitUntilExit()
                let data = output.fileHandleForReading.readDataToEndOfFile()
                let text = String(data: data, encoding: .utf8)?
                    .split(separator: "\n")
                    .suffix(2)
                    .joined(separator: " ")
                DispatchQueue.main.async {
                    if announce {
                        self.commandRunning = false
                        self.commandMessage = text?.isEmpty == false ? text! : "Command finished"
                    }
                    self.refresh()
                }
            } catch {
                DispatchQueue.main.async {
                    if announce {
                        self.commandRunning = false
                        self.commandMessage = error.localizedDescription
                    }
                    self.refresh()
                }
            }
        }
    }

    func openRepo() {
        NSWorkspace.shared.open(repoURL)
    }

    func openLog() {
        NSWorkspace.shared.open(logURL)
    }

    func revealBridgeHelper() {
        let url = URL(fileURLWithPath: NSHomeDirectory())
            .appendingPathComponent("Applications/StickS3Bridge.app")
        NSWorkspace.shared.activateFileViewerSelecting([url])
    }

    static func compact(_ value: UInt64) -> String {
        let units: [(Double, String)] = [
            (1_000_000_000, "B"),
            (1_000_000, "M"),
            (1_000, "K")
        ]
        let doubleValue = Double(value)
        for (divisor, suffix) in units where doubleValue >= divisor {
            let scaled = doubleValue / divisor
            return scaled >= 100 ? "\(Int(round(scaled)))\(suffix)" : String(format: "%.1f%@", scaled, suffix)
        }
        return "\(value)"
    }
}

struct ContentView: View {
    @ObservedObject var model: BridgeModel

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            header
            Divider()
            statusBlock
            usageBlock
            commandBlock
            Divider()
            actionButtons
            secondaryButtons
        }
        .padding(14)
        .frame(width: 360)
    }

    private var header: some View {
        HStack(spacing: 10) {
            Image(systemName: model.symbolName)
                .foregroundStyle(color(for: model.menuTitle))
            VStack(alignment: .leading, spacing: 2) {
                Text("StickS3 Companion")
                    .font(.headline)
                Text(model.menuTitle)
                    .font(.caption)
                    .foregroundStyle(color(for: model.menuTitle))
            }
            Spacer()
            Button {
                model.refresh()
            } label: {
                Image(systemName: "arrow.clockwise")
            }
            .buttonStyle(.borderless)
            .help("Refresh")
        }
    }

    private var statusBlock: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(model.statusLine)
                .font(.system(.body, design: .monospaced))
                .lineLimit(4)
                .textSelection(.enabled)
            HStack {
                Text("\(model.codexLabel) · \(model.deviceLabel)")
                Spacer()
                Text("PID \(model.payload?.supervisorPid.map(String.init) ?? model.payload?.pid.map(String.init) ?? "-")")
            }
            .font(.caption)
            .foregroundStyle(.secondary)
            if let deviceError = model.payload?.deviceError, !deviceError.isEmpty {
                Text("S3 \(deviceError)")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
            }
            if let thread = model.payload?.threadName ?? model.payload?.threadId {
                Text("Thread \(thread)")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
        }
    }

    private var usageBlock: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("TOK")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Text(model.tokensLabel)
                    .font(.system(.body, design: .monospaced).weight(.semibold))
                Spacer()
                Text(model.payload?.updatedAt ?? "not updated")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            UsageBar(title: "5h", limit: model.payload?.rateLimits?.primary)
            UsageBar(title: "7d", limit: model.payload?.rateLimits?.secondary)
        }
    }

    private var commandBlock: some View {
        Group {
            if model.commandRunning {
                HStack {
                    ProgressView()
                        .controlSize(.small)
                    Text(model.commandMessage)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            } else if !model.commandMessage.isEmpty {
                Text(model.commandMessage)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
            }
        }
    }

    private var actionButtons: some View {
        HStack(spacing: 8) {
            Button {
                model.runBridgeCommand("start")
            } label: {
                Label("Start", systemImage: "play.fill")
            }
            Button {
                model.runBridgeCommand("stop")
            } label: {
                Label("Stop", systemImage: "stop.fill")
            }
            Button {
                model.runBridgeCommand("restart")
            } label: {
                Label("Restart", systemImage: "arrow.clockwise")
            }
        }
        .disabled(model.commandRunning)
    }

    private var secondaryButtons: some View {
        HStack(spacing: 8) {
            Button {
                model.openLog()
            } label: {
                Label("Log", systemImage: "doc.text")
            }
            Button {
                model.openRepo()
            } label: {
                Label("Repo", systemImage: "folder")
            }
            Button {
                model.revealBridgeHelper()
            } label: {
                Label("Helper", systemImage: "app")
            }
            Spacer()
            Button("Quit") {
                NSApplication.shared.terminate(nil)
            }
        }
    }

    private func color(for title: String) -> Color {
        switch model.codexState {
        case "work", "working":
            return .cyan
        case "err", "error":
            return .red
        case "idle", "connected":
            return .green
        case "wait", "starting":
            return .orange
        default:
            return .secondary
        }
    }
}

struct UsageBar: View {
    let title: String
    let limit: RateLimit?

    private var remaining: Int? {
        limit?.remainingPercent
    }

    var body: some View {
        HStack(spacing: 8) {
            Text(title)
                .font(.caption.monospaced())
                .frame(width: 22, alignment: .leading)
            GeometryReader { proxy in
                ZStack(alignment: .leading) {
                    RoundedRectangle(cornerRadius: 3)
                        .fill(Color.secondary.opacity(0.18))
                    RoundedRectangle(cornerRadius: 3)
                        .fill(barColor)
                        .frame(width: proxy.size.width * CGFloat(max(0, min(remaining ?? 0, 100))) / 100)
                }
            }
            .frame(height: 7)
            Text(remaining.map { "\($0)%" } ?? "n/a")
                .font(.caption.monospaced())
                .frame(width: 42, alignment: .trailing)
        }
    }

    private var barColor: Color {
        guard let remaining else { return .secondary.opacity(0.45) }
        if remaining <= 20 { return .red }
        if remaining <= 45 { return .orange }
        return .green
    }
}

@main
struct StickS3CompanionApp: App {
    @StateObject private var model = BridgeModel()

    var body: some Scene {
        MenuBarExtra {
            ContentView(model: model)
        } label: {
            Text(model.menuTitle)
                .font(.system(size: 12, weight: .semibold, design: .monospaced))
        }
        .menuBarExtraStyle(.window)
    }
}
