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
- Always use `/swarm` as the default execution mode for coding, debugging, verification, and release tasks.
- Select worker models (`gemini-3.8-flash-*`, `gemini-4-argon-*`, `claude-sonnet-5-5-*`, `claude-opus-5-5-*`) and reasoning effort levels (`low`, `medium`, `high`) strictly based on task complexity and need:
  - **Simple / Routine Tasks (docs, version bumps, release packaging, build/test verifiers, straightforward fixes)**:
    - Prefer `gemini-3.8-flash-low` or `gemini-3.8-flash-medium` (use `gemini-3.8-flash-high` for multi-file edits or thorough test verification).
  - **Medium-to-Complex Feature Development & Refactoring (UI components, Win32/GDI+ rendering, networking logic)**:
    - Use `gemini-4-argon-low`, `gemini-4-argon-medium`, or `gemini-4-argon-high` according to reasoning depth required.
  - **High-Complexity Architecture, Subtle Race Conditions & Security-Critical Reviews**:
    - Use `claude-sonnet-5-5` (`low`/`medium`/`high`) or `claude-opus-5-5` (`low`/`medium`/`high`) sparingly.
    - Because Claude models (`claude-opus-5-5` and `claude-sonnet-5-5`) have very small quota limits, do not use them for routine work; reserve them for the hardest tasks and always check `swarm{action:"models"}` quota first.
- Always scale reasoning tier (`low` -> `medium` -> `high`) to the minimum level necessary for reliable completion.

### 5. Release Management & GitHub Releases Text
- Never publish empty or auto-generated release notes: Every version release must have a formatted title and changelog body matching past releases (`v2.0.0`, `v2.0.1`, `v2.0.2`).
- Release Title Format: `QuickDNS vX.Y.Z - <Highlights summary>` (e.g. `QuickDNS v2.0.2 - Security Hardening, SHA-256 Update Verification, IPv6 DNS & Architecture Stability`).
- Release Body Structure:
  - Heading: `### QuickDNS vX.Y.Z`
  - Lead summary paragraph explaining core focus of release.
  - Bulleted change breakdown with bold topic names: `- **<Topic>**: <What changed and technical reason>`.
  - Full changelog compare link at bottom: `**Full Changelog**: https://github.com/szanko02/QuickDNS/compare/v<prev>...v<new>`.
  - Maintain `RELEASE_NOTES.md` in repository root with bilingual (English primary, Russian summary) text before tagging.
- Workflow & Post-Release Verification:
  - After publishing tag or running CI, verify GitHub Release via API or web to ensure title, body, and all binary assets (`QuickDNS.exe`, `QuickDNS.exe.sha256`, `QuickDNS-vX.Y.Z-Portable.zip`) are attached.
  - If existing release was drafted or auto-created without body, patch title and body immediately via GitHub API (`PATCH /repos/{owner}/{repo}/releases/{id}`).
  - Never allow dangling or cancelled CI runs to remain unnoticed; cancel stalled runs and monitor completion.
