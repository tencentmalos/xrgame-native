#!/usr/bin/env node
"use strict";

const crypto = require("crypto");
const fs = require("fs");
const path = require("path");

let sharp;
try {
  sharp = require("sharp");
} catch (error) {
  console.error(
    "sharp is required. Load Codex workspace dependencies and set NODE_PATH to their node_modules directory."
  );
  throw error;
}

function option(name, fallback = null) {
  const index = process.argv.indexOf(name);
  return index >= 0 && index + 1 < process.argv.length ? process.argv[index + 1] : fallback;
}

const input = option("--input");
const output = option("--output");
const width = Number.parseInt(option("--width", "1840"), 10);
if (!input || !output || !Number.isFinite(width) || width <= 0) {
  throw new Error("usage: render_stage_preview.cjs --input timeline.svg --output preview.png [--width 1840]");
}

const outputDirectory = path.dirname(output);
fs.mkdirSync(outputDirectory, { recursive: true });
const temporary = path.join(outputDirectory, `.${path.basename(output)}.${process.pid}.tmp`);

async function main() {
  try {
    await sharp(input, { density: 144 })
      .resize({ width, withoutEnlargement: false })
      .png({ compressionLevel: 9, adaptiveFiltering: true })
      .toFile(temporary);
    fs.renameSync(temporary, output);
    const payload = fs.readFileSync(output);
    console.log(
      JSON.stringify({
        png: output,
        bytes: payload.length,
        sha256: crypto.createHash("sha256").update(payload).digest("hex"),
        width,
      })
    );
  } finally {
    if (fs.existsSync(temporary)) fs.unlinkSync(temporary);
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
