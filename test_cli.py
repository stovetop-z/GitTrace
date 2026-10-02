"""Integration checks for the GitTrace command-line entry point."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

CLI_EXECUTABLE = None
PROJECT_ROOT = None


class GitTraceCliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.executable = Path(CLI_EXECUTABLE).resolve()
        cls.project_root = Path(PROJECT_ROOT).resolve()

    def run_cli(self, *args):
        return subprocess.run(
            [str(self.executable), *map(str, args)],
            cwd=self.project_root,
            text=True,
            capture_output=True,
            check=False,
        )

    def make_repository(self, root):
        repository = root / "sample-repository"
        repository.mkdir()
        commands = [
            ["git", "-C", str(repository), "init", "-q"],
            ["git", "-C", str(repository), "config", "user.name", "CLI Test"],
            ["git", "-C", str(repository), "config", "user.email", "cli-test@example.invalid"],
        ]
        for command in commands:
            subprocess.run(command, check=True, capture_output=True, text=True)

        (repository / "probe.txt").write_text("known CLI fixture content\n", encoding="utf-8")
        subprocess.run(
            ["git", "-C", str(repository), "add", "probe.txt"],
            check=True,
            capture_output=True,
            text=True,
        )
        subprocess.run(
            ["git", "-C", str(repository), "commit", "-q", "-m", "CLI fixture"],
            check=True,
            capture_output=True,
            text=True,
        )
        return repository

    def test_help_shows_cli_options(self):
        result = self.run_cli("--help")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--repo PATH", result.stdout)
        self.assertIn("--top-k N", result.stdout)

    def test_rejects_invalid_top_k_before_scanning(self):
        result = self.run_cli("--top-k", "0")
        self.assertEqual(result.returncode, 2)
        self.assertIn("--top-k requires a positive integer", result.stderr)

    def test_scans_explicit_custom_repository(self):
        with tempfile.TemporaryDirectory(prefix="gittrace-cli-test-") as temp_dir:
            repository = self.make_repository(Path(temp_dir))
            result = self.run_cli(
                "--repo", repository,
                "--no-embeddings",
                "--no-interactive",
                "--list",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Opening repository:", result.stdout)
        self.assertIn("probe.txt", result.stdout)
        self.assertIn("Collected 1 unique text blob version(s)", result.stdout)
        self.assertIn("Embeddings were skipped", result.stdout)


if __name__ == "__main__":
    # unittest treats command-line arguments as its own options; keep only the
    # two arguments used to locate the built executable and project root.
    CLI_EXECUTABLE, PROJECT_ROOT = sys.argv[1:3]
    sys.argv = [sys.argv[0]]
    unittest.main()
