/**
 * Merge Original Scripts
 *
 * Reads every decoded script in `decoded_txt/`, detects inline speech
 * patterns, and writes a single `merged-original.txt` in UTF-8.
 *
 * Speech is inline: `speaker「content」`. A known speaker set distinguishes
 * speech from narration that happens to contain 「」.
 *
 * Detected speech lines become two lines:
 *
 *   ＃{speaker}
 *   「{content}」
 *
 * Everything else (narration, choices, chapter titles) is kept as-is.
 * The `=== filename.doj ===` header and empty lines are removed.
 *
 * File sections are separated by `--------------------` and each section
 * starts with the filename followed by `********************`.
 *
 * Usage:
 *   node merge-original-scripts.mjs
 */

import { glob } from "glob";
import { mkdir, readFile, rm, writeFile } from "fs/promises";
import path from "path";

const INPUT_DIR = "decoded_txt";
const OUTPUT_FILE = "merged-original.txt";

const SECTION_SEPARATOR = "--------------------";
const HEADER_SEPARATOR = "********************";

const MAX_CHUNK_LINES = 900;
const CHUNKS_DIR = "original-merged-chunks";

// Known speaker names used to detect inline speech patterns.
const KNOWN_SPEAKERS = new Set([
  "葵",
  "秋人",
  "流一",
  "沙莉亜",
  "正田",
  "冬樹",
  "福井",
  "光輝",
  "桜子",
  "鳴海",
  "？",
  "女子学生",
  "ジョージ",
  "男子学生",
  "責任者",
  "少女",
  "教師",
  "女子学生Ａ",
  "ヤクザ",
  "女子学生Ｂ",
  "男子学生Ａ",
  "司会者",
  "委員長",
  "担任教師",
  "テニス部員",
  "茅",
  "男",
  "葵の母",
  "保健医",
  "友人",
  "女子",
  "女の子",
  "友達Ａ",
  "通行人３",
  "黒服の男",
  "友人Ａ",
  "友人Ｂ",
  "女子１",
  "女子２",
  "女子３",
  "体育教師",
  "女",
  "顧問",
  "通行人１",
  "通行人２",
  "男子学生Ｂ",
  "男子学生Ｃ",
  "男子学生Ｄ",
  "男子学生衆",
  "柔道部員",
  "バスケ部員",
  "サッカー部員",
  "ラグビー部員",
  "男子部員たち",
  "神田",
]);

// Matches speaker「content」 where speaker is at the start of the line.
const SPEECH_PATTERN = /^(.+?)(「[\s\S]*」)$/;

/**
 * Try to parse an inline speech line into { speaker, content }.
 * Returns null if the line is narration.
 */
function parseSpeech(line) {
  const match = line.match(SPEECH_PATTERN);
  if (!match) return null;

  const speaker = match[1];
  const content = match[2];

  // Only treat as speech if the speaker is in the known set.
  if (!KNOWN_SPEAKERS.has(speaker)) return null;

  return { speaker, content };
}

async function main() {
  // Step 1: Discover all text files in the input directory.
  const files = (await glob(`${INPUT_DIR}/*.txt`)).sort();

  if (files.length === 0) {
    console.error(`No .txt files found in ${INPUT_DIR}/`);
    process.exit(1);
  }

  const sections = [];

  for (const filePath of files) {
    // Step 2: Read each file (already UTF-8).
    const fileName = path.basename(filePath, ".txt");
    const raw = await readFile(filePath, "utf-8");
    const srcLines = raw.split("\n");
    if (srcLines.at(-1) === "") srcLines.pop();

    // Step 3: Process each line — skip headers, remove empties, parse speech.
    const lines = [];
    for (const srcLine of srcLines) {
      const trimmed = srcLine.trim();

      // Skip the doj file header (e.g. "=== 01c01.doj ===").
      if (trimmed.startsWith("===") && trimmed.endsWith("===")) continue;

      // Skip empty lines.
      if (trimmed.length === 0) continue;

      // Try to parse inline speech (speaker「content」).
      // Speech lines don't have leading spaces, so trimmed is fine for parsing.
      const speech = parseSpeech(trimmed);
      if (speech) {
        lines.push(`＃${speech.speaker}`);
        lines.push(speech.content);
      } else {
        // Preserve leading whitespace (e.g. full-width space 　) from the original.
        lines.push(srcLine);
      }
    }

    // Step 4: Build the section with a filename header.
    sections.push(`${fileName}\n${HEADER_SEPARATOR}\n${lines.join("\n")}`);
  }

  // Step 5: Prepend each section with a separator and write to disk.
  const output = sections.map((s) => `${SECTION_SEPARATOR}\n${s}`).join("\n");
  await writeFile(OUTPUT_FILE, output + "\n", "utf-8");

  console.log(`${files.length} files merged into ${OUTPUT_FILE}`);

  // Step N: Split sections into line-limited chunks.
  await rm(CHUNKS_DIR, { recursive: true, force: true });
  await mkdir(CHUNKS_DIR, { recursive: true });

  const chunks = [];
  let currentChunk = [];
  let currentLineCount = 0;

  for (const section of sections) {
    const sectionText = `${SECTION_SEPARATOR}\n${section}`;
    const sectionLineCount = sectionText.split("\n").length;

    // If adding this section exceeds the limit and we already have content,
    // flush the current chunk first.
    if (currentLineCount + sectionLineCount > MAX_CHUNK_LINES && currentChunk.length > 0) {
      chunks.push(currentChunk);
      currentChunk = [];
      currentLineCount = 0;
    }

    currentChunk.push(sectionText);
    currentLineCount += sectionLineCount;
  }

  if (currentChunk.length > 0) {
    chunks.push(currentChunk);
  }

  for (let i = 0; i < chunks.length; i++) {
    const chunkNum = String(i + 1).padStart(3, "0");
    const chunkPath = path.join(CHUNKS_DIR, `part-${chunkNum}.txt`);
    await writeFile(chunkPath, chunks[i].join("\n") + "\n", "utf-8");
  }

  console.log(`${chunks.length} chunks written to ${CHUNKS_DIR}/`);
}

main().catch(console.error);
