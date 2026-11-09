# Full `git grep` output for scenarios A–D

Each linked text file contains the complete, unmodified stdout from the indicated command. The searches ran against `HEAD` (`d77c3ccebcd22970ed4b21c58d35a810b60fa72b`) and exited with status 0. `HEAD:` prefixes are part of Git's output because each command specifies the revision.

| Scenario | Remembered keywords | Command | Complete output |
|---|---|---|---|
| A | older, historical, revision | `git grep -n -I -i -E 'older|historical|revision' HEAD` | [git-grep-scenario-A.txt](../git-grep-scenario-A.txt) |
| B | ignore, exclude | `git grep -n -I -i -E 'ignore|exclude' HEAD` | [git-grep-scenario-B.txt](../git-grep-scenario-B.txt) |
| C | prompt, instruction, untrusted | `git grep -n -I -i -E 'prompt|instruction|untrusted' HEAD` | [git-grep-scenario-C.txt](../git-grep-scenario-C.txt) |
| D | timeout, fallback, rerank | `git grep -n -I -i -E 'timeout|fallback|rerank' HEAD` | [git-grep-scenario-D.txt](../git-grep-scenario-D.txt) |

`git grep` returns every matching line and does not rank by relevance. The `-E` alternatives match if any listed term appears on a line. For a Google Forms comparison, show participants the exact keywords and set a stopping rule (for example, inspect the first 10 matches or inspect all matches); GitTrace's top-*k* results are otherwise not directly comparable to an unlimited grep output.
