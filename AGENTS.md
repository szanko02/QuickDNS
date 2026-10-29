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
