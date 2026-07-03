// swift-tools-version: 6.0

import PackageDescription

let package = Package(
    name: "M5StackCodexCompanion",
    platforms: [
        .macOS(.v13)
    ],
    products: [
        .executable(name: "M5StackCodexCompanion", targets: ["M5StackCodexCompanion"])
    ],
    targets: [
        .executableTarget(
            name: "M5StackCodexCompanion",
            path: "Sources"
        )
    ]
)
