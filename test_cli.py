"""Integration checks for the GitTrace command-line entry point."""

import subprocess
import sys
import tempfile
import unittest
import json
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
        self.assertIn("--retrieval-mode M", result.stdout)
        self.assertIn("--query-expansion", result.stdout)
        self.assertIn("--query TEXT", result.stdout)
        self.assertIn("--profile PATH", result.stdout)
        self.assertIn("--rebuild-index", result.stdout)
        self.assertIn("--index-status", result.stdout)
        self.assertIn("--index PATH", result.stdout)

    def test_rejects_invalid_top_k_before_scanning(self):
        result = self.run_cli("--top-k", "0")
        self.assertEqual(result.returncode, 2)
        self.assertIn("--top-k requires a positive integer", result.stderr)

    def test_rejects_invalid_retrieval_mode_before_scanning(self):
        result = self.run_cli("--retrieval-mode", "unknown")
        self.assertEqual(result.returncode, 2)
        self.assertIn("--retrieval-mode must be vector, keyword, or hybrid", result.stderr)

    def test_rejects_query_when_embeddings_are_disabled(self):
        result = self.run_cli("--no-embeddings", "--query", "find an implementation")
        self.assertEqual(result.returncode, 2)
        self.assertIn("--query requires embeddings to be enabled", result.stderr)

    def test_scans_explicit_custom_repository(self):
        with tempfile.TemporaryDirectory(prefix="gittrace-cli-test-") as temp_dir:
            repository = self.make_repository(Path(temp_dir))
            log_path = self.project_root / "dev.log"
            log_offset = log_path.stat().st_size if log_path.exists() else 0
            result = self.run_cli(
                "--repo", repository,
                "--no-embeddings",
                "--no-interactive",
                "--list",
            )
            with log_path.open("r", encoding="utf-8", errors="replace") as log_file:
                log_file.seek(log_offset)
                initialization_log = log_file.read()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Loading...", result.stdout)
        self.assertIn("Loading complete. See dev.log", result.stdout)
        self.assertNotIn("Opening repository:", result.stdout)
        self.assertIn("Opening repository:", initialization_log)
        self.assertIn("probe.txt", initialization_log)
        self.assertIn("Indexed 1 unique text file state(s)", initialization_log)
        self.assertIn("Embeddings were skipped", initialization_log)

    def test_writes_machine_readable_profile(self):
        with tempfile.TemporaryDirectory(prefix="gittrace-profile-test-") as temp_dir:
            root = Path(temp_dir)
            repository = self.make_repository(root)
            profile = root / "profile.json"
            result = self.run_cli(
                "--repo", repository,
                "--no-embeddings",
                "--no-interactive",
                "--profile", profile,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(profile.read_text(encoding="utf-8"))

        self.assertEqual(report["unique_file_versions"], 1)
        self.assertEqual(report["commits_visited"], 1)
        for field in ("init_ms", "history_walk_ms", "grammar_prepare_ms", "parsing_ms"):
            self.assertGreaterEqual(report[field], 0)
        self.assertEqual(report["query_total_ms"], [])

    def test_index_status_does_not_build_or_load_the_model(self):
        with tempfile.TemporaryDirectory(prefix="gittrace-index-status-test-") as temp_dir:
            repository = self.make_repository(Path(temp_dir))
            index_path = Path(temp_dir) / "custom-index.sqlite"
            result = self.run_cli("--repo", repository, "--index-status", "--index", index_path)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("No local index", result.stdout)
        self.assertIn(str(index_path), result.stdout)


if __name__ == "__main__":
    # unittest treats command-line arguments as its own options; keep only the
    # two arguments used to locate the built executable and project root.
    CLI_EXECUTABLE, PROJECT_ROOT = sys.argv[1:3]
    sys.argv = [sys.argv[0]]
    unittest.main()
