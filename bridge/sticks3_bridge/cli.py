from __future__ import annotations

import argparse
import asyncio
import logging
from pathlib import Path

from .app_server import CodexAppServerBridge, build_transport
from .desktop_observer import DesktopObserverBridge, default_codex_home
from .device import BleStickS3Device, FakeStickS3Device


def add_device_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--device-prefix", default="Codex-S3-")
    parser.add_argument("--address", help="BLE address to connect directly instead of scanning.")
    parser.add_argument("--scan-timeout", type=float, default=10.0)
    parser.add_argument(
        "--fake-device",
        action="store_true",
        help="Do not use BLE; log snapshots instead.",
    )


def build_device(args: argparse.Namespace) -> FakeStickS3Device | BleStickS3Device:
    return (
        FakeStickS3Device(auto_decision=getattr(args, "auto_decision", "deny"))
        if args.fake_device
        else BleStickS3Device(
            device_prefix=args.device_prefix,
            address=args.address,
            scan_timeout=args.scan_timeout,
        )
    )


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
    add_device_args(app_server)
    app_server.add_argument("--approval-timeout", type=float, default=300.0)
    app_server.add_argument("--auto-decision", choices=["once", "session", "deny", "cancel"], default="deny")

    desktop_observer = subcommands.add_parser(
        "desktop-observer",
        help="Read local Codex Desktop rollout logs and mirror status to the StickS3 over BLE.",
    )
    add_device_args(desktop_observer)
    desktop_observer.add_argument(
        "--codex-home",
        help="Codex home directory. Defaults to CODEX_HOME or ~/.codex.",
    )
    desktop_observer.add_argument(
        "--sessions-dir",
        help="Codex sessions directory. Defaults to <codex-home>/sessions.",
    )
    desktop_observer.add_argument(
        "--thread-id",
        help="Specific Codex thread id to observe. Defaults to the freshest non-subagent Desktop rollout.",
    )
    desktop_observer.add_argument(
        "--rollout",
        help="Specific rollout JSONL file to observe.",
    )
    desktop_observer.add_argument("--poll-interval", type=float, default=2.0)
    desktop_observer.add_argument("--heartbeat-interval", type=float, default=10.0)
    desktop_observer.add_argument(
        "--status-file",
        help="Write desktop-observer lifecycle/status JSON to this file for menu bar helpers.",
    )

    return parser


async def run_app_server(args: argparse.Namespace) -> None:
    transport = await build_transport(args.transport, args.target, args.stdio_command)
    device = build_device(args)

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


async def run_desktop_observer(args: argparse.Namespace) -> None:
    device = build_device(args)
    codex_home = Path(args.codex_home).expanduser() if args.codex_home else default_codex_home()
    sessions_dir = Path(args.sessions_dir).expanduser() if args.sessions_dir else codex_home / "sessions"
    rollout_path = Path(args.rollout).expanduser() if args.rollout else None

    bridge = DesktopObserverBridge(
        device=device,
        sessions_dir=sessions_dir,
        rollout_path=rollout_path,
        thread_id=args.thread_id,
        poll_interval=args.poll_interval,
        heartbeat_interval=args.heartbeat_interval,
        status_file=Path(args.status_file).expanduser() if args.status_file else None,
    )

    try:
        await bridge.run()
    except Exception as exc:
        bridge.write_status("error", error=str(exc))
        raise
    finally:
        await device.close()


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

    if args.command == "desktop-observer":
        try:
            asyncio.run(run_desktop_observer(args))
        except KeyboardInterrupt:
            return 130
        except Exception as exc:
            logging.error("%s", exc)
            return 1
        return 0

    parser.error(f"unsupported command: {args.command}")
    return 2
