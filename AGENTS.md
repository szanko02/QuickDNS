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

### 4. /subagents & /boost
- State routine first: Solo or Delegation.
- Use Delegation routine for coding and investigation tasks:
  - Delegate coding to DeepCoder (`invoke_subagent` with `TypeName='DeepCoder'`, `Workspace='inherit'`).
  - Delegate investigation to DeepInvestigator (`invoke_subagent` with `TypeName='DeepInvestigator'`, `Workspace='inherit'`).
  - No pre-work before first subagent call.
  - Independently verify results before accepting. Spawn additional rounds if needed.
- Use Solo routine only for simple tasks and meta-configuration.

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
