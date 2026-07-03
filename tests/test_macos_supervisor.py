import json
import os
import runpy
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


class MacosSupervisorTests(unittest.TestCase):
    def load_supervisor(self, tmp: Path) -> dict:
        repo = tmp / "repo"
        runtime = tmp / "runtime"
        home = tmp / "home"
        repo.mkdir()
        runtime.mkdir()
        home.mkdir()
        script = Path(__file__).resolve().parents[1] / "scripts" / "sticks3-macos-bridge"
        with patch.dict(os.environ, {"HOME": str(home), "STICKS3_REPO": str(repo), "STICKS3_RUNTIME_DIR": str(runtime)}):
            return runpy.run_path(str(script))

    def test_write_status_clears_stale_codex_work_fields(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            module = self.load_supervisor(Path(name))
            status_file = module["STATUS_FILE"]
            status_file.write_text(
                json.dumps(
                    {
                        "active": True,
                        "codex_state": "work",
                        "menu_mode": "work",
                        "mode": "work",
                        "status": {"speaker": "Tool", "kind": "started", "text": "exec_command"},
                        "task_active": True,
                        "tokens": 1234,
                    }
                ),
                encoding="utf-8",
            )

            module["write_status"]("stopped", detail="Bridge is not running")

            payload = json.loads(status_file.read_text(encoding="utf-8"))
            self.assertEqual("off", payload["codex_state"])
            self.assertEqual("off", payload["mode"])
            self.assertEqual("off", payload["menu_mode"])
            self.assertFalse(payload["active"])
            self.assertFalse(payload["task_active"])
            self.assertEqual({"speaker": "System", "kind": "stopped", "text": "Bridge is not running"}, payload["status"])
            self.assertEqual(1234, payload["tokens"])

    def test_supervisor_uses_m5stack_agent_names(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            module = self.load_supervisor(Path(name))

            self.assertEqual("com.simon.m5stack-codex-companion.bridge", module["AGENT_LABEL"])
            self.assertEqual("M5Stack Codex Companion", module["APP_SUPPORT_DIR"].name)
            self.assertEqual(".M5StackCodexBridge.app", module["AGENT_APP_DIR"].name)
            self.assertEqual("M5StackCodexBridgeLauncher", module["APP_EXEC"].name)
            self.assertEqual("M5StackCodexBridge.app", module["VISIBLE_AGENT_APP_DIR"].name)

    def test_generated_launcher_embeds_python_in_helper_process(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            module = self.load_supervisor(Path(name))

            source = module["app_launcher_source"]()

            self.assertIn("#include <Python.h>", source)
            self.assertIn("PYTHONHOME", source)
            self.assertIn("PYTHONEXECUTABLE", source)
            self.assertIn("Py_BytesMain", source)
            self.assertIn("Py_BytesMain(argc, argv)", source)
            self.assertNotIn("exec ", source)

    def test_supervisor_starts_helper_executable_directly(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            module = self.load_supervisor(Path(name))
            app_dir = Path(name) / ".M5StackCodexBridge.app"
            module["app_open_command"].__globals__["ensure_agent_app"] = lambda: app_dir
            args = module["build_parser"]().parse_args(["start"])

            command = module["app_open_command"](args)

            self.assertEqual(str(app_dir / "Contents" / "MacOS" / "M5StackCodexBridgeLauncher"), command[0])
            self.assertEqual(str(app_dir / "Contents" / "Resources" / "bridge_app_main.py"), command[1])
            self.assertNotIn("/bin/zsh", command)

    def test_launch_agent_starts_helper_executable_directly(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            module = self.load_supervisor(Path(name))
            app_dir = Path(name) / ".M5StackCodexBridge.app"
            module["agent_payload"].__globals__["ensure_agent_app"] = lambda: app_dir
            args = module["build_parser"]().parse_args(["install-agent"])

            payload = module["agent_payload"](args)

            self.assertEqual(
                str(app_dir / "Contents" / "MacOS" / "M5StackCodexBridgeLauncher"),
                payload["ProgramArguments"][0],
            )
            self.assertEqual(str(app_dir / "Contents" / "Resources" / "bridge_app_main.py"), payload["ProgramArguments"][1])
            self.assertNotIn("/bin/zsh", payload["ProgramArguments"])

    def test_agent_app_support_is_marked_private_for_spotlight(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            module = self.load_supervisor(Path(name))

            module["mark_support_dir_private"]()

            self.assertTrue(module["SPOTLIGHT_EXCLUDE_FILE"].exists())

    def test_supervisor_migrates_legacy_pairing_store_by_move(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            module = self.load_supervisor(Path(name))
            legacy_path = module["LEGACY_PAIRING_STORE_PATH"]
            new_path = module["PAIRING_STORE_PATH"]
            legacy_path.parent.mkdir(parents=True)
            legacy_path.write_text(json.dumps({"host_id": "old-host", "devices": []}), encoding="utf-8")

            migrated = module["migrate_legacy_support"]()

            self.assertTrue(migrated)
            self.assertFalse(legacy_path.exists())
            self.assertEqual({"host_id": "old-host", "devices": []}, json.loads(new_path.read_text(encoding="utf-8")))

    def test_supervisor_migration_does_not_overwrite_new_pairing_store(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            module = self.load_supervisor(Path(name))
            legacy_path = module["LEGACY_PAIRING_STORE_PATH"]
            new_path = module["PAIRING_STORE_PATH"]
            legacy_path.parent.mkdir(parents=True)
            new_path.parent.mkdir(parents=True)
            legacy_path.write_text(json.dumps({"host_id": "old-host", "devices": []}), encoding="utf-8")
            new_path.write_text(json.dumps({"host_id": "new-host", "devices": []}), encoding="utf-8")

            migrated = module["migrate_legacy_support"]()

            self.assertFalse(migrated)
            self.assertTrue(legacy_path.exists())
            self.assertEqual({"host_id": "new-host", "devices": []}, json.loads(new_path.read_text(encoding="utf-8")))


if __name__ == "__main__":
    unittest.main()
