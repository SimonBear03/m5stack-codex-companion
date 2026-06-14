// swift-tools-version: 6.0

import PackageDescription

let package = Package(
    name: "StickS3Companion",
    platforms: [
        .macOS(.v13)
    ],
    products: [
        .executable(name: "StickS3Companion", targets: ["StickS3Companion"])
    ],
    targets: [
        .executableTarget(
            name: "StickS3Companion",
            path: "Sources"
        )
    ]
)
