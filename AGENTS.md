# Local co-op development rules

- Work on `coop/campaign-foundation` or another explicitly chosen feature branch.
  Keep upstream `master` unchanged.
- Every new task gets its own commit after its relevant build, acceptance tests
  and regression checks pass. Do not leave a completed task uncommitted or combine
  several completed tasks into a later commit.
- Commit all engine source and documentation changes for the task. Include the
  task identifier and meaningful validation in the commit message.
- The sibling tools/project-notes repository is the parent directory of this
  checkout. If the task changes its tools, launchers or notes, commit those
  changes there as well before reporting completion. Follow its `AGENTS.md`,
  `COOP_TASKS.md` and `TESTING.md` when working in this workspace.
- The user has authorized local task-completion commits. Do not ask for renewed
  confirmation for them. Pushing or publishing requires separate authorization.
- Inspect status and diffs before staging; preserve unrelated user changes.
  Verify repository status afterward and report the branch and commit IDs.
- Keep game assets, saves, logs, build products and generated `game/build.h`
  out of Git. Autonomous GUI tests use isolated runtimes, small windows, a
  30-FPS target and 50% render scale; stop their owned processes when finished.
- Run tests silently, including effects, dialogue, videos and music. The parent
  repository's runner and test launcher apply `tools/prepare-gothic-test.py` to
  each isolated runtime before launch. Enable audio only for a user-requested
  audio test; preserve normal-play audio settings and system volume.
- The first co-op commit is the existing baseline through N02. Every subsequent
  task follows the separate-commit rule above.
