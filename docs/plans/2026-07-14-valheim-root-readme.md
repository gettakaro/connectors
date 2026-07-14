# Valheim Root README Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add the merged Valheim connector to the root repository README and make the release instructions match the repository's current automation and build recipes.

**Architecture:** Keep the root README as a concise connector index. Add Valheim alongside the existing connectors and link to `valheim/README.md` for installation, companion, capability, and operational details rather than duplicating them.

**Tech Stack:** Markdown, Just, Release Please

---

### Task 1: Update the root connector index

**Files:**
- Modify: `README.md`

**Step 1: Add the missing Valheim build recipe**

Add this recipe after the other connector build recipes:

```just
# Build the Valheim connector release artifacts locally into <out-dir>
build-release-valheim version out-dir='dist':
    ./valheim/scripts/build-release.sh {{version}} {{out-dir}}
```

**Step 2: Add Valheim to the connector table**

Add this row after Terraria:

```markdown
| Valheim | [`valheim/`](valheim/) | C# / .NET | BepInEx dedicated-server plugin and graphical-client companion |
```

**Step 3: Correct the release documentation**

Describe the Release Please workflow already configured by the repository. Replace the obsolete `release-*` commands with the valid local artifact-build recipes, including:

```bash
just build-release-valheim 2.0.0
```

**Step 4: Add the architecture note**

Add a concise connector note linking to `valheim/README.md` and explaining that Takaro/cloud credentials remain on the dedicated server while client-owned gameplay observations use the separately packaged companion.

### Task 2: Verify and commit the documentation

**Files:**
- Verify: `README.md`

**Step 1: Verify references and formatting**

Run: `test -d valheim && test -f valheim/README.md && rg -n '^build-release-(rust|minecraft|7d2d|conan|terraria|valheim)' justfile && rg -n 'Valheim|build-release-valheim|valheim/README.md|Release Please' README.md && git diff --check`

Expected: all referenced paths and the release recipe exist, the README contains each approved addition, and `git diff --check` exits successfully.

**Step 2: Review the scoped diff**

Run: `git diff -- README.md`

Expected: only the approved connector row, corrected release section, connector note, and Valheim Just recipe are added.

**Step 3: Commit**

```bash
git add README.md justfile
git commit -m "docs(valheim): list connector in root readme"
```
