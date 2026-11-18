# QuickDNS Agent Guidelines & Active Modes

These rules are persistent for this project. Apply on every prompt.

## Active Modes

### 1. /caveman
- Respond terse like smart caveman.
- Keep all technical facts. Drop fluff, greeting, pleasantries, ceremony.
- Answer first, then reason, then next step.
- Drop articles when clear. One idea per sentence.
- Code blocks and paths unchanged.
- Strictly no emojis anywhere (chat, commits, code, documentation).

### 2. /browser
- Use web search and browser inspection for external docs, APIs, streaming sources, and verification when needed.

### 3. /ub-mode
- Reduced principles for code changes:
  1. First Make it Work: smallest edit, TDD, model domain, idempotent, subtract before add.
  2. Then Make it Right: reproduce, fix root cause, sequence in verifiable units.
  3. Follow Practice: match codebase style, maintain tests, clean boundaries.

### 4. /swarm & Dynamic Model / Reasoning Selection
- Always use `/swarm` or `/smart-swarm` as the default execution mode for coding, debugging, verification, and release tasks.
- Always pass an explicit `model` on every `swarm{action:"add", ...}` call using the 6-tier model hierarchy from highest to lowest (`Carbon > Argon > Opus > Hydrogen > Sonnet > 3.8 Flash`):
  - **Supreme / Coordinator / Multi-Subsystem Architecture**:
    - `gemini-4-carbon-low`, `gemini-4-carbon-medium`, or `gemini-4-carbon-high`.
  - **Critical System / Maximum Complexity Architecture**:
    - `gemini-4-argon-low`, `gemini-4-argon-medium`, or `gemini-4-argon-high`.
  - **Hardest Bugs / Subtle Concurrency / Deep Analysis**:
    - `claude-opus-4-6-thinking` (or `claude-opus-5-5-low`, `claude-opus-5-5-medium`, `claude-opus-5-5-high` when available; check `swarm{action:"models"}` quota first).
  - **Web Research / Codebase Inspection / Medium-Complex Features**:
    - `gemini-4-hydrogen-low`, `gemini-4-hydrogen-medium`, `gemini-4-hydrogen-high`, or `gemini-4-hydrogen-xhigh`.
  - **Simple Edits (docs, INF, config, simple 1-file edits)**:
    - `claude-sonnet-4-6` (or `claude-sonnet-5-5-low`, `claude-sonnet-5-5-medium`, `claude-sonnet-5-5-high` when available).
  - **Trivial / Baseline (`monitor`, `narrator`, trivial release checks)**:
    - `gemini-3.8-flash-low`, `gemini-3.8-flash-medium`, or `gemini-3.8-flash-high`.
- Always scale reasoning tier (`low` -> `medium` -> `high` -> `xhigh` where supported) to the minimum level necessary for reliable completion.

### 5. Release Management & GitHub Releases Text
- Never publish empty or auto-generated release notes: Every version release must have a formatted title and changelog body matching past releases (`v2.0.0`, `v2.0.1`, `v2.0.2`, `v2.1.0`).
- Release Title Format: `QuickDNS vX.Y.Z - <Highlights summary>` (e.g. `QuickDNS v2.0.2 - Security Hardening, SHA-256 Update Verification, IPv6 DNS & Architecture Stability`).
- Release Body Structure:
  - Heading: `### QuickDNS vX.Y.Z - <Highlights summary>` (extracted dynamically by `.github/workflows/build.yml` as the GitHub Release title).
  - Lead summary paragraph explaining core focus of release.
  - Bulleted change breakdown with bold topic names: `- **<Topic>**: <What changed and technical reason>`.
  - Bilingual Russian summary section (`#### Описание изменений (Русский)`).
  - Full changelog compare link at bottom: `**Full Changelog**: https://github.com/szanko02/QuickDNS/compare/v<prev>...v<new>`.
  - Maintain `RELEASE_NOTES.md` in repository root with bilingual (English primary, Russian summary) text before tagging.
- Workflow & Post-Release Verification:
  - Tags not reachable from `origin/main` or carrying pre-release suffixes (`-rc*`, `-beta*`) must always be marked `prerelease: true` and `make_latest: false`.
  - After publishing tag or running CI, verify GitHub Release via API or web to ensure title, body, and all binary assets (`QuickDNS.exe`, `QuickDNS.exe.sha256`, `QuickDNS-vX.Y.Z-Portable.zip`) are attached.
  - If existing release was drafted or auto-created without body, patch title and body immediately via GitHub API (`PATCH /repos/{owner}/{repo}/releases/{id}`).
  - Never allow dangling or cancelled CI runs to remain unnoticed; cancel stalled runs and monitor completion.
