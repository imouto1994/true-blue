/**
 * Export Translation Map
 *
 * Reads original and translated merged chunks, parses them into matching
 * sections, and builds a JSON mapping of every unique original line to its
 * translated counterpart.
 *
 * Speech source lines (＃ in original, $ in translated) and their following
 * content lines are merged into a single entry:
 *
 *   Original:  ＃少女         →  key:   "少女「……っ」"
 *              「……っ」        value: "Girl: \"...!\""
 *
 * Narration lines are mapped directly:
 *
 *   key:   "　――都内某所。"
 *   value: "--Somewhere in Tokyo."
 *
 * Empty lines are skipped. First occurrence wins for duplicates.
 *
 * Output: `translation-map.json`
 *
 * Usage:
 *   node export-translation-map.mjs
 */

import { readFile, writeFile } from "fs/promises";
import { glob } from "glob";

const ORIGINAL_CHUNKS_DIR = "original-merged-chunks";
const TRANSLATED_CHUNKS_DIR = "translated-merged-chunks";
const OUTPUT_FILE = "translation-map.json";

/**
 * Read and concatenate all chunk files from a directory.
 */
async function readChunks(dir) {
  const files = (await glob(`${dir}/part-*.txt`)).sort();
  const parts = await Promise.all(files.map((f) => readFile(f, "utf-8")));
  return parts.join("\n");
}

const SECTION_SEPARATOR = "--------------------";
const HEADER_SEPARATOR = "********************";

const SPEAKER_MAP = new Map([
  ["葵", "Aoi"],
  ["秋人", "Akito"],
  ["流一", "Ryuuichi"],
  ["沙莉亜", "Saria"],
  ["正田", "Shouda"],
  ["冬樹", "Fuyuki"],
  ["福井", "Fukui"],
  ["光輝", "Kouki"],
  ["桜子", "Sakurako"],
  ["鳴海", "Narumi"],
  ["？", "?"],
  ["女子学生", "Female Student"],
  ["ジョージ", "George"],
  ["男子学生", "Male Student"],
  ["責任者", "Manager"],
  ["少女", "Girl"],
  ["教師", "Teacher"],
  ["女子学生Ａ", "Female Student A"],
  ["ヤクザ", "Yakuza"],
  ["女子学生Ｂ", "Female Student B"],
  ["男子学生Ａ", "Male Student A"],
  ["司会者", "Emcee"],
  ["委員長", "Class President"],
  ["担任教師", "Homeroom Teacher"],
  ["テニス部員", "Tennis Club Member"],
  ["茅", "Kaya"],
  ["男", "Man"],
  ["葵の母", "Aoi's Mother"],
  ["保健医", "School Nurse"],
  ["友人", "Friend"],
  ["女子", "Girl"],
  ["女の子", "Little Girl"],
  ["友達Ａ", "Friend A"],
  ["通行人３", "Passerby 3"],
  ["黒服の男", "Man in Black"],
  ["友人Ａ", "Friend A"],
  ["友人Ｂ", "Friend B"],
  ["女子１", "Girl 1"],
  ["女子２", "Girl 2"],
  ["女子３", "Girl 3"],
  ["体育教師", "PE Teacher"],
  ["女", "Woman"],
  ["顧問", "Advisor"],
  ["通行人１", "Passerby 1"],
  ["通行人２", "Passerby 2"],
  ["男子学生Ｂ", "Male Student B"],
  ["男子学生Ｃ", "Male Student C"],
  ["男子学生Ｄ", "Male Student D"],
  ["男子学生衆", "Male Students"],
  ["柔道部員", "Judo Club Member"],
  ["バスケ部員", "Basketball Club Member"],
  ["サッカー部員", "Soccer Club Member"],
  ["ラグビー部員", "Rugby Club Member"],
  ["男子部員たち", "Male Club Members"],
  ["神田", "Kanda"],
]);

/**
 * Parse a merged text file into a Map of { fileName → lines[] },
 * preserving empty lines so indices stay aligned between original and
 * translated.
 */
function parseSections(text) {
  const raw = text.split(`${SECTION_SEPARATOR}\n`);
  const sections = new Map();

  for (const block of raw) {
    const headerEnd = block.indexOf(`\n${HEADER_SEPARATOR}\n`);
    if (headerEnd === -1) continue;

    const fileName = block.slice(0, headerEnd).trim();
    const body = block.slice(headerEnd + HEADER_SEPARATOR.length + 2);

    sections.set(fileName, body.split("\n"));
  }

  return sections;
}

async function main() {
  // Step 1: Read and concatenate all chunks from both directories.
  const originalText = await readChunks(ORIGINAL_CHUNKS_DIR);
  const translatedText = await readChunks(TRANSLATED_CHUNKS_DIR);

  // Step 2: Parse into section maps keyed by filename.
  const origSections = parseSections(originalText);
  const transSections = parseSections(translatedText);

  const map = new Map();
  let totalPairs = 0;
  let duplicates = 0;
  const unknownSpeakers = new Set();

  // Step 3: Walk through each section, pairing original and translated lines.
  for (const [fileName, origLines] of origSections) {
    if (!transSections.has(fileName)) continue;
    const transLines = transSections.get(fileName);

    let i = 0;
    while (i < origLines.length && i < transLines.length) {
      const origLine = origLines[i];
      const transLine = transLines[i];

      // Step 3a: Skip empty lines.
      if (origLine.length === 0) {
        i++;
        continue;
      }

      // Step 3b: Handle speech lines (＃ source + content on next line).
      // Original uses full-width ＃, translated uses $.
      if (origLine.startsWith("＃")) {
        const speakerJP = origLine.slice(1);
        const speakerEN = SPEAKER_MAP.get(speakerJP);

        if (!speakerEN) {
          unknownSpeakers.add(speakerJP);
        }

        // Merge speaker + content into a single map entry.
        if (i + 1 < origLines.length && i + 1 < transLines.length) {
          const contentOrig = origLines[i + 1];
          const contentTrans = transLines[i + 1];

          // Key uses inline format: speaker name + bracketed content.
          const key = `${speakerJP}${contentOrig}`;
          const value = `${speakerEN || speakerJP}: ${contentTrans}`;

          if (!map.has(key)) {
            map.set(key, value);
            totalPairs++;
          } else {
            duplicates++;
          }

          i += 2;
        } else {
          i++;
        }
        continue;
      }

      // Step 3c: Handle narration lines — map original directly to translated.
      if (!map.has(origLine)) {
        map.set(origLine, transLine);
        totalPairs++;
      } else {
        duplicates++;
      }

      i++;
    }
  }

  // Step 4: Write the translation map to disk as JSON.
  const obj = Object.fromEntries(map);
  await writeFile(OUTPUT_FILE, JSON.stringify(obj, null, 2), "utf-8");

  // Step 5: Print summary.
  console.log("— Summary —");
  console.log(`  Sections processed: ${origSections.size}`);
  console.log(`  Unique entries:     ${totalPairs}`);
  console.log(`  Duplicates skipped: ${duplicates}`);
  console.log(`  Exported to:        ${OUTPUT_FILE}`);

  if (unknownSpeakers.size > 0) {
    console.log(
      `\n  Unknown speakers: ${[...unknownSpeakers].join(", ")}`,
    );
  }
}

main().catch(console.error);
