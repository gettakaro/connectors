#!/usr/bin/env node
// This attestation is produced only after the private, complete live proof has been reviewed.
// Run with --source-digest on the exact release candidate to record its source identity.
import { createHash } from "node:crypto";
import { readFileSync, existsSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import path from "node:path";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../../..");
const game = path.join(root, "games/enshrouded");
const checklist = JSON.parse(readFileSync(path.join(game, "release-checklist.json"), "utf8"));
const hex = /^[a-f0-9]{64}$/;

function sourceDigest() {
  const names = execFileSync("git", ["ls-files", "-z", "--", "games/enshrouded", "catalog/enshrouded"], { cwd: root })
    .toString("utf8").split("\0").filter(Boolean)
    .filter((name) => !/\/(release-proof\.json|version\.txt|CHANGELOG\.md)$/.test(name))
    .sort();
  const h = createHash("sha256");
  for (const name of names) {
    h.update(name).update("\0");
    h.update(readFileSync(path.join(root, name))).update("\0");
  }
  return h.digest("hex");
}

if (process.argv.includes("--source-digest")) {
  console.log(sourceDigest());
  process.exit(0);
}

const errors = [];
const requireValue = (ok, message) => { if (!ok) errors.push(message); };
const proofPath = path.join(game, "release-proof.json");
if (!existsSync(proofPath)) {
  console.error("Enshrouded live release proof is absent; stable release is blocked.");
  process.exit(1);
}
const proof = JSON.parse(readFileSync(proofPath, "utf8"));
const target = JSON.parse(readFileSync(path.join(root, "catalog/enshrouded/targets/proton-1269579.json"), "utf8"));
const manifest = JSON.parse(readFileSync(path.join(root, ".release-please-manifest.json"), "utf8"));
requireValue(proof.schemaVersion === 1, "unknown proof schema");
requireValue(proof.target === target.id && proof.serverBuild === target.revision, "proof target/server build differs from release target");
requireValue(proof.clientBuild === target.revision, "Windows client build differs from server build");
requireValue(proof.version === manifest["games/enshrouded"], "proof version differs from release version");
requireValue(proof.sourceDigest === sourceDigest(), "proof source digest differs from this candidate");
requireValue(hex.test(proof.artifactSha256 ?? ""), "release artifact SHA-256 is missing");
requireValue(hex.test(proof.proofJsonSha256 ?? ""), "private proof JSON SHA-256 is missing");
requireValue(/^https:\/\/claude\.ai\/artifact\//.test(proof.proofPage ?? ""), "private proof page link is missing");
requireValue(/^context\/games\/enshrouded\/evidence\/[a-zA-Z0-9_-]+\/proof\.json$/.test(proof.proofRun ?? ""), "private proof run path is missing");
const required = Object.values(checklist.supported).flat();
const ids = new Set();
for (const c of proof.checks ?? []) {
  requireValue(required.includes(c.id), `unexpected check: ${c.id}`);
  requireValue(!ids.has(c.id), `duplicate check: ${c.id}`);
  ids.add(c.id);
  requireValue(c.status === "PASS", `${c.id}: status must be PASS`);
  requireValue(hex.test(c.rawSha256 ?? ""), `${c.id}: raw Takaro JSON digest missing`);
  if (checklist.clientScreenshotRequired.includes(c.id))
    requireValue(hex.test(c.clientScreenshotSha256 ?? ""), `${c.id}: client screenshot digest missing`);
}
for (const id of required) requireValue(ids.has(id), `missing supported check: ${id}`);
requireValue(JSON.stringify(proof.excluded) === JSON.stringify(checklist.excluded), "excluded checks/reasons differ from checklist");
if (errors.length) {
  for (const error of errors) console.error(`release proof: ${error}`);
  process.exit(1);
}
console.log(`Enshrouded live proof: ${required.length}/${required.length} PASS, source and artifact identified`);
