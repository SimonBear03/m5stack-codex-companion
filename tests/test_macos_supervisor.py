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
        repo.mkdir()
        runtime.mkdir()
        script = Path(__file__).resolve().parents[1] / "scripts" / "sticks3-macos-bridge"
        with patch.dict(os.environ, {"STICKS3_REPO": str(repo), "STICKS3_RUNTIME_DIR": str(runtime)}):
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


if __name__ == "__main__":
    unittest.main()
