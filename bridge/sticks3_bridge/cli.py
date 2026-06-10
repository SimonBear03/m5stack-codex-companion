from __future__ import annotations

import argparse
import asyncio
import logging

from .app_server import CodexAppServerBridge, build_transport
from .device import BleStickS3Device, FakeStickS3Device


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="sticks3-bridge")
    parser.add_argument("--log-level", default="INFO", choices=["DEBUG", "INFO", "WARNING", "ERROR"])

    subcommands = parser.add_subparsers(dest="command", required=True)

    app_server = subcommands.add_parser(
        "app-server",
        help="Bridge a Codex App-compatible app-server endpoint to the StickS3 over BLE.",
    )
    app_server.add_argument(
        "--transport",
        choices=["ws", "stdio"],
        default="ws",
        help="Use ws for a running macOS app-server endpoint, or stdio for a spawned validation server.",
    )
    app_server.add_argument(
        "--target",
        help="WebSocket endpoint, for example ws://127.0.0.1:4567 or unix:///path/to/socket.",
    )
    app_server.add_argument(
        "--stdio-command",
        default="codex app-server --stdio",
        help="Command used when --transport stdio is selected.",
    )
    app_server.add_argument("--device-prefix", default="Codex-S3-")
    app_server.add_argument("--address", help="BLE address to connect directly instead of scanning.")
    app_server.add_argument("--scan-timeout", type=float, default=10.0)
    app_server.add_argument("--approval-timeout", type=float, default=300.0)
    app_server.add_argument(
        "--fake-device",
        action="store_true",
        help="Do not use BLE; log snapshots and auto-answer approval prompts.",
    )
    app_server.add_argument("--auto-decision", choices=["once", "session", "deny", "cancel"], default="deny")

    return parser


async def run_app_server(args: argparse.Namespace) -> None:
    transport = await build_transport(args.transport, args.target, args.stdio_command)
    device = (
        FakeStickS3Device(auto_decision=args.auto_decision)
        if args.fake_device
        else BleStickS3Device(
            device_prefix=args.device_prefix,
            address=args.address,
            scan_timeout=args.scan_timeout,
        )
    )

    bridge = CodexAppServerBridge(
        transport=transport,
        device=device,
        approval_timeout=args.approval_timeout,
    )

    try:
        await bridge.run()
    finally:
        await device.close()
        await transport.close()


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    logging.basicConfig(level=getattr(logging, args.log_level), format="%(levelname)s %(message)s")

    if args.command == "app-server":
        try:
            asyncio.run(run_app_server(args))
        except KeyboardInterrupt:
            return 130
        except Exception as exc:
            logging.error("%s", exc)
            return 1
        return 0

    parser.error(f"unsupported command: {args.command}")
    return 2
