# Valheim Root README Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add the merged Valheim connector to the root repository README, including its release command and architecture note.

**Architecture:** Keep the root README as a concise connector index. Add Valheim alongside the existing connectors and link to `valheim/README.md` for installation, companion, capability, and operational details rather than duplicating them.

**Tech Stack:** Markdown, Just

---

### Task 1: Update the root connector index

**Files:**
- Modify: `README.md`

**Step 1: Confirm the documented release recipe exists**

Run: `rg -n '^release-valheim' justfile`

Expected: one `release-valheim` recipe is present.

**Step 2: Add Valheim to the connector table**

Add this row after Terraria:

```markdown
| Valheim | [`valheim/`](valheim/) | C# / .NET | BepInEx dedicated-server plugin and graphical-client companion |
```

**Step 3: Add the release command**

Add this command after the Terraria release example:

```bash
just release-valheim 1.0.0     # Tags valheim-v1.0.0 and pushes
```

**Step 4: Add the architecture note**

Add a concise connector note linking to `valheim/README.md` and explaining that Takaro/cloud credentials remain on the dedicated server while client-owned gameplay observations use the separately packaged companion.

### Task 2: Verify and commit the documentation

**Files:**
- Verify: `README.md`

**Step 1: Verify references and formatting**

Run: `test -d valheim && test -f valheim/README.md && rg -n '^release-valheim' justfile && rg -n 'Valheim|release-valheim|valheim/README.md' README.md && git diff --check`

Expected: all referenced paths and the release recipe exist, the README contains each approved addition, and `git diff --check` exits successfully.

**Step 2: Review the scoped diff**

Run: `git diff -- README.md`

Expected: only the approved connector row, release command, and connector note are added.

**Step 3: Commit**

```bash
git add README.md
git commit -m "docs(valheim): list connector in root readme"
```
