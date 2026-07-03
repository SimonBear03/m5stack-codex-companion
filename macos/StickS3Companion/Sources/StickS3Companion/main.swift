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

struct CodexActivity: Decodable {
    let status: String?
    let title: String?
    let subtitle: String?
    let waitingKind: String?
    let level: String?
    let isLoading: Bool?
    let updatedAt: Int?
    let expiresAt: Int?
    let priority: Int?
    let threadLabel: String?
    let projectLabel: String?

    enum CodingKeys: String, CodingKey {
        case status
        case title
        case subtitle
        case waitingKind = "waiting_kind"
        case level
        case isLoading = "is_loading"
        case updatedAt = "updated_at"
        case expiresAt = "expires_at"
        case priority
        case threadLabel = "thread_label"
        case projectLabel = "project_label"
    }
}

struct CompanionDevice: Decodable {
    let deviceId: String?
    let label: String?
    let board: String?
    let name: String?
    let address: String?
    let state: String?
    let error: String?
    let lastSeen: String?
    let detailMode: Int?

    init(
        deviceId: String? = nil,
        label: String? = nil,
        board: String? = nil,
        name: String? = nil,
        address: String? = nil,
        state: String? = nil,
        error: String? = nil,
        lastSeen: String? = nil,
        detailMode: Int? = nil
    ) {
        self.deviceId = deviceId
        self.label = label
        self.board = board
        self.name = name
        self.address = address
        self.state = state
        self.error = error
        self.lastSeen = lastSeen
        self.detailMode = detailMode
    }

    enum CodingKeys: String, CodingKey {
        case deviceId = "device_id"
        case label
        case board
        case name
        case address
        case state
        case error
        case lastSeen = "last_seen"
        case detailMode = "detail_mode"
    }

    var displayName: String {
        firstNonEmpty(label, name, shortDeviceId, boardLabel) ?? "Companion"
    }

    var boardLabel: String {
        switch board {
        case "sticks3":
            return "StickS3"
        case "cardputer_adv":
            return "Cardputer ADV"
        case let value? where !value.isEmpty:
            return value
        default:
            return ""
        }
    }

    var stateLabel: String {
        switch normalizedState {
        case "connected":
            return "Connected"
        case "scanning", "connecting":
            return "Scanning"
        case "disconnected":
            return "Disconnected"
        case "error":
            return "Error"
        case let value where !value.isEmpty:
            return value.capitalized
        default:
            return "Unknown"
        }
    }

    var detailLabel: String? {
        switch detailMode {
        case 0:
            return "Full"
        case 1:
            return "Status"
        case 2:
            return "Usage"
        default:
            return nil
        }
    }

    var normalizedState: String {
        state?.lowercased() ?? "unknown"
    }

    var subtitle: String {
        [boardLabel, name == label ? nil : name, detailLabel.map { "Detail \($0)" }]
            .compactMap { value in
                guard let value, !value.isEmpty else { return nil }
                return value
            }
            .joined(separator: " · ")
    }

    private var shortDeviceId: String? {
        guard let deviceId, !deviceId.isEmpty else { return nil }
        return String(deviceId.prefix(10))
    }

    private func firstNonEmpty(_ values: String?...) -> String? {
        for value in values {
            if let value, !value.isEmpty {
                return value
            }
        }
        return nil
    }
}

struct BridgePayload: Decodable {
    let active: Bool?
    let bridgeState: String?
    let codexState: String?
    let detail: String?
    let deviceCount: Int?
    let deviceError: String?
    let deviceState: String?
    let devices: [CompanionDevice]?
    let connectedDeviceCount: Int?
    let codexActivity: CodexActivity?
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
        case deviceCount = "device_count"
        case deviceError = "device_error"
        case deviceState = "device_state"
        case devices
        case connectedDeviceCount = "connected_device_count"
        case codexActivity = "codex_activity"
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
    let bridgeCLIURL: URL

    private var timer: Timer?

    init() {
        let repoPath = BridgeModel.resolveRepoPath()
        repoURL = URL(fileURLWithPath: repoPath, isDirectory: true)
        scriptURL = repoURL.appendingPathComponent("scripts/sticks3-macos-bridge")
        statusURL = repoURL.appendingPathComponent("runtime/bridge-status.json")
        logURL = repoURL.appendingPathComponent("runtime/bridge.log")
        bridgeCLIURL = repoURL.appendingPathComponent(".bridge-venv/bin/sticks3-bridge")
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
        "\(codexLabel) · \(deviceSummaryLabel)"
    }

    var compactMenuTitle: String {
        "\(shortCodexLabel) \(connectedDeviceCount)/\(deviceCount)"
    }

    private var shortCodexLabel: String {
        switch codexState {
        case "work", "working", "running":
            return "Work"
        case "idle", "connected":
            return "Idle"
        case "err", "error", "failed":
            return "Err"
        case "wait", "starting", "waiting":
            return "Wait"
        case "review":
            return "Review"
        default:
            return codexState.prefix(1).uppercased() + codexState.dropFirst()
        }
    }

    var symbolName: String {
        switch codexState {
        case "work", "working", "running":
            return "bolt.horizontal.circle.fill"
        case "err", "error", "failed":
            return "exclamationmark.triangle.fill"
        case "idle", "connected":
            return "checkmark.circle.fill"
        case "wait", "starting", "waiting":
            return "clock.circle.fill"
        case "review":
            return "checkmark.seal.fill"
        default:
            return "circle"
        }
    }

    var codexState: String {
        guard let payload else { return "wait" }
        if let status = payload.codexActivity?.status, !status.isEmpty {
            return status
        }
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

    var companionDevices: [CompanionDevice] {
        if let devices = payload?.devices, !devices.isEmpty {
            return devices
        }
        guard payload?.deviceCount == nil, let legacy = legacyCompanionDevice else {
            return []
        }
        return [legacy]
    }

    var deviceCount: Int {
        payload?.deviceCount ?? companionDevices.count
    }

    var connectedDeviceCount: Int {
        payload?.connectedDeviceCount ?? companionDevices.filter { $0.normalizedState == "connected" }.count
    }

    var deviceSummaryLabel: String {
        if deviceCount <= 0 {
            return "BLE 0/0"
        }
        return "BLE \(connectedDeviceCount)/\(deviceCount)"
    }

    private var legacyCompanionDevice: CompanionDevice? {
        guard let payload else { return nil }
        let state = deviceState
        if ["off", "stopped"].contains(state) {
            return nil
        }
        return CompanionDevice(
            label: "Companion",
            state: state,
            error: payload.deviceError ?? payload.error,
            lastSeen: payload.updatedAt
        )
    }

    var codexLabel: String {
        switch codexState {
        case "work", "working", "running":
            return "Codex Work"
        case "idle", "connected":
            return "Codex Idle"
        case "err", "error", "failed":
            return "Codex Err"
        case "wait", "starting", "waiting":
            return "Codex Wait"
        case "review":
            return "Codex Review"
        default:
            return "Codex \(codexState.capitalized)"
        }
    }

    var statusLine: String {
        if let activity = payload?.codexActivity {
            let title = activity.title?.trimmingCharacters(in: .whitespacesAndNewlines)
            let subtitle = activity.subtitle?.trimmingCharacters(in: .whitespacesAndNewlines)
            if let title, let subtitle, !title.isEmpty, !subtitle.isEmpty {
                return "\(title): \(subtitle)"
            }
            if let subtitle, !subtitle.isEmpty {
                return subtitle
            }
            if let title, !title.isEmpty {
                return title
            }
        }
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
            process.executableURL = URL(fileURLWithPath: "/usr/bin/python3")
            process.arguments = [script.path, command]
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

    func pairCardputer() {
        runPairCommand(label: "Cardputer", prefix: "Codex-CP-")
    }

    func pairStickS3() {
        runPairCommand(label: "StickS3", prefix: "Codex-S3-")
    }

    private func runPairCommand(label: String, prefix: String) {
        guard !commandRunning else { return }
        guard FileManager.default.isExecutableFile(atPath: bridgeCLIURL.path) else {
            commandMessage = "Bridge CLI is missing at \(bridgeCLIURL.path)"
            return
        }

        commandRunning = true
        commandMessage = "Pairing \(label)... confirm on device."

        let bridgeCLI = bridgeCLIURL
        let script = scriptURL
        let repo = repoURL
        DispatchQueue.global(qos: .userInitiated).async {
            let pairResult = Self.runProcess(
                executable: bridgeCLI,
                arguments: [
                    "--log-level", "INFO",
                    "pair-device",
                    "--device-prefix", prefix,
                    "--scan-timeout", "30",
                    "--label", label
                ],
                currentDirectory: repo
            )

            var message = Self.summarizeOutput(pairResult.output)
            if pairResult.exitCode == 0 {
                let restartResult = Self.runProcess(
                    executable: URL(fileURLWithPath: "/usr/bin/python3"),
                    arguments: [script.path, "restart"],
                    currentDirectory: repo
                )
                let restartSummary = Self.summarizeOutput(restartResult.output)
                if restartResult.exitCode == 0 {
                    message = message.isEmpty ? "Paired \(label)" : message
                } else {
                    message = restartSummary.isEmpty ? "Paired \(label), but restart failed" : restartSummary
                }
            } else if message.isEmpty {
                message = "Pairing \(label) failed"
            }

            DispatchQueue.main.async {
                self.commandRunning = false
                self.commandMessage = message
                self.refresh()
            }
        }
    }

    nonisolated private static func runProcess(executable: URL, arguments: [String], currentDirectory: URL) -> (exitCode: Int32, output: String) {
        let process = Process()
        process.executableURL = executable
        process.arguments = arguments
        process.currentDirectoryURL = currentDirectory
        process.environment = [
            "PATH": "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin",
            "STICKS3_REPO": currentDirectory.path
        ]

        let output = Pipe()
        process.standardOutput = output
        process.standardError = output

        do {
            try process.run()
            process.waitUntilExit()
            let data = output.fileHandleForReading.readDataToEndOfFile()
            return (process.terminationStatus, String(data: data, encoding: .utf8) ?? "")
        } catch {
            return (1, error.localizedDescription)
        }
    }

    nonisolated private static func summarizeOutput(_ output: String) -> String {
        output
            .split(separator: "\n")
            .filter { !$0.trimmingCharacters(in: .whitespaces).isEmpty }
            .suffix(2)
            .joined(separator: " ")
    }

    func openRepo() {
        NSWorkspace.shared.open(repoURL)
    }

    func openLog() {
        NSWorkspace.shared.open(logURL)
    }

    func revealBridgeHelper() {
        let url = URL(fileURLWithPath: NSHomeDirectory())
            .appendingPathComponent("Library/Application Support/M5Stack Codex Companion/M5StackCodexBridge.app")
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
            companionsBlock
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
                Text("M5Stack Codex Companion")
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
                Text("\(model.codexLabel) · \(model.deviceSummaryLabel)")
                Spacer()
                Text("PID \(model.payload?.supervisorPid.map(String.init) ?? model.payload?.pid.map(String.init) ?? "-")")
            }
            .font(.caption)
            .foregroundStyle(.secondary)
            if let deviceError = model.payload?.deviceError, !deviceError.isEmpty, model.companionDevices.isEmpty {
                Text("Companion \(deviceError)")
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

    private var companionsBlock: some View {
        VStack(alignment: .leading, spacing: 7) {
            HStack {
                Text("Companions")
                    .font(.caption.weight(.semibold))
                Spacer()
                Text("\(model.connectedDeviceCount)/\(model.deviceCount) connected")
                    .font(.caption.monospaced())
                    .foregroundStyle(.secondary)
            }
            if model.companionDevices.isEmpty {
                Text("No paired companions")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else {
                VStack(alignment: .leading, spacing: 6) {
                    ForEach(Array(model.companionDevices.enumerated()), id: \.offset) { _, device in
                        CompanionDeviceRow(device: device)
                    }
                }
            }
            HStack(spacing: 8) {
                Button {
                    model.pairCardputer()
                } label: {
                    Label("Pair Cardputer", systemImage: "keyboard.badge.ellipsis")
                }
                Button {
                    model.pairStickS3()
                } label: {
                    Label("Pair StickS3", systemImage: "display.badge.plus")
                }
            }
            .disabled(model.commandRunning)
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
        case "work", "working", "running":
            return .cyan
        case "err", "error", "failed":
            return .red
        case "idle", "connected":
            return .green
        case "wait", "starting", "waiting":
            return .orange
        case "review":
            return .green
        default:
            return .secondary
        }
    }
}

struct CompanionDeviceRow: View {
    let device: CompanionDevice

    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: symbolName)
                .foregroundStyle(stateColor)
                .frame(width: 14)
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 8) {
                    Text(device.displayName)
                        .font(.caption.weight(.semibold))
                        .lineLimit(1)
                    Spacer()
                    Text(device.stateLabel)
                        .font(.caption.monospaced())
                        .foregroundStyle(stateColor)
                }
                if !device.subtitle.isEmpty {
                    Text(device.subtitle)
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
                if let error = device.error, !error.isEmpty {
                    Text(error)
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .lineLimit(2)
                        .truncationMode(.middle)
                } else if let lastSeen = device.lastSeen, !lastSeen.isEmpty {
                    Text("Last seen \(lastSeen)")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
            }
        }
    }

    private var symbolName: String {
        switch device.normalizedState {
        case "connected":
            return "antenna.radiowaves.left.and.right.circle.fill"
        case "scanning", "connecting":
            return "dot.radiowaves.left.and.right"
        case "error":
            return "exclamationmark.triangle.fill"
        default:
            return "circle"
        }
    }

    private var stateColor: Color {
        switch device.normalizedState {
        case "connected":
            return .green
        case "scanning", "connecting":
            return .orange
        case "error":
            return .red
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
struct M5StackCodexCompanionApp: App {
    @StateObject private var model = BridgeModel()

    var body: some Scene {
        MenuBarExtra {
            ContentView(model: model)
        } label: {
            HStack(spacing: 4) {
                Image(systemName: model.symbolName)
                Text(model.compactMenuTitle)
                    .font(.system(size: 12, weight: .semibold, design: .monospaced))
            }
        }
        .menuBarExtraStyle(.window)
    }
}
