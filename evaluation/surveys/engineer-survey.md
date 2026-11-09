# Engineer survey: GitTrace compared with `git grep`

**Participant copy — do not include the separate researcher answer key.**

## Study purpose

This short exercise asks how useful GitTrace and a conventional `git grep` search are for locating implementation evidence in a codebase. We are evaluating the tools and their results, not your performance. There may be tasks where neither result is useful.

Estimated time: 15–20 minutes.

## How to use the result cards

For each task, review the prepared GitTrace results and the matching `git grep` output. The complete outputs are linked below. GitTrace shows its top five ranked chunks. `git grep` returns every line matching a fixed set of remembered keywords; it does not rank matches. Review the available output before answering. Record the file, line, and commit you relied on, if any.

The GitTrace output uses hybrid retrieval with query expansion and Laya/Gemini reranking disabled. It indexes the repository history using the local `.gittraceignore` rules shown below. `git grep` searches the `HEAD` snapshot. This is the comparison setup for this exercise.

| Scenario | Task card with both tools' complete outputs |
|---|---|
| A | [Older revisions](scenario-a-search.txt) |
| B | [Ignore rules in history](scenario-b-search.txt) |
| C | [Reranker instructions](scenario-c-search.txt) |
| D | [Unavailable reranker](scenario-d-search.txt) |

The GitTrace result file contains the four tasks in A–D order. Use the matching `Query:` heading. If reproducing the run rather than reviewing prepared output, use repository revision `d77c3ccebcd22970ed4b21c58d35a810b60fa72b`, a Release build, and top 5 hybrid results with `--no-laya` and query expansion off. The index contains 239 chunks. The active `.gittraceignore` is:

```gitignore
LICENSE
README*
docs/
*.py
!laya_service.py
*.txt
*.json
*.jsonl
```

## Before the tasks

1. How often do you use `git grep` or similar command-line code search?
   - Daily
   - Weekly
   - Monthly
   - Less than monthly
   - Never

2. How familiar are you with searching Git history?
   - Not familiar
   - Slightly familiar
   - Moderately familiar
   - Very familiar
   - Expert

3. What is your primary role? (Optional short answer.)

## For each task

Review both result sets. Fill out one copy of this rating block for **each** tool so the ratings stay comparable.

### GitTrace / `git grep` (select the tool being rated)

1. Did this tool provide enough evidence to answer the task? (Yes / Partly / No / Unsure)
2. How useful were the results? (1 = Not useful, 2 = Slightly useful, 3 = Moderately useful, 4 = Very useful, 5 = Extremely useful.)
3. How confident are you that the result you selected supports your answer? (1 = Not confident, 5 = Very confident.)
4. Which file, line range, and commit did you rely on? If none, write “none.”
5. How many results or matching lines did you inspect before deciding?
6. About how many seconds did it take you to decide?

After rating both tools for the task:

7. Which tool helped more on this task?
   - GitTrace
   - `git grep`
   - About the same
   - Neither

8. What most influenced your choice? (Optional paragraph.)

## Task A — Older revisions

**Question:** What calculation downweights chunks from older revisions during search?

**`git grep` command:**

```sh
git grep -n -I -i -E 'older|historical|revision' HEAD
```

Complete one rating block for GitTrace and one for `git grep`, then answer the comparison question.

## Task B — Ignore rules in history

**Question:** Are repository-local ignore rules applied to files throughout the commit history walk?

**`git grep` command:**

```sh
git grep -n -I -i -E 'ignore|exclude' HEAD
```

Complete one rating block for GitTrace and one for `git grep`, then answer the comparison question.

## Task C — Reranker instructions

**Question:** Can a retrieved code snippet override Gemini reranking instructions?

**`git grep` command:**

```sh
git grep -n -I -i -E 'prompt|instruction|untrusted' HEAD
```

Complete one rating block for GitTrace and one for `git grep`, then answer the comparison question.

## Task D — Reranker unavailable

**Question:** If the relevance reranker cannot be reached, what does the CLI display?

**`git grep` command:**

```sh
git grep -n -I -i -E 'timeout|fallback|rerank' HEAD
```

Complete one rating block for GitTrace and one for `git grep`, then answer the comparison question.

## Overall questions

1. Across all four tasks, which tool would you choose first for a similar search?
   - GitTrace
   - `git grep`
   - It depends on the task
   - Neither

2. Which tool was more helpful when you did not remember the exact identifier or wording?
   - GitTrace
   - `git grep`
   - About the same
   - Neither

3. Which tool was more helpful for finding exact words or identifiers?
   - GitTrace
   - `git grep`
   - About the same
   - Neither

4. How useful were historical versions in the results? (1 = They made results much less useful, 3 = No effect, 5 = They made results much more useful.)

5. Did duplicate or historical results make it harder to identify the current implementation? (Never, Rarely, Sometimes, Often, Always.)

6. What is the biggest limitation you noticed in either tool? (Optional paragraph.)

7. What change would most increase your willingness to use GitTrace? (Optional paragraph.)

8. Any other comments? (Optional paragraph.)

## Facilitator notes — remove before posting if desired

- To reduce order effects in a live session, alternate which tool is reviewed first across participants. Do not show the researcher answer key before responses are collected.
- Keep the query wording, GitTrace top-five depth, `.gittraceignore`, index revision, and GitTrace settings fixed across participants.
- The supplied GitTrace result card is a prepared run, so self-reported seconds measure time to inspect the card, not end-to-end command startup or indexing. If measuring live workflow time, start timing before the participant enters the query and keep the index prebuilt.
- `git grep` is intentionally given the fixed remembered-keyword alternatives shown above. This makes the baseline reproducible but does not represent every search strategy an engineer might try. Record any deviations separately.
- GitTrace searches an eight-commit index; the supplied `git grep` commands search `HEAD` only. Interpret answers about historical code with that scope difference in mind.
