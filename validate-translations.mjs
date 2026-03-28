/**
 * Validate Translations (chunk-based)
 *
 * Compares translated chunks in `translated-merged-chunks/` against original
 * chunks in `original-merged-chunks/` to ensure structural consistency.
 *
 * Checks performed per section:
 *   1. Every original section has a matching translated section (by filename).
 *   2. Non-empty line counts match.
 *   3. Line types match (source / speech / choice / normal).
 *   4. Speech source names match via SPEAKER_MAP (JP → EN).
 *   5. Choice markers and choice items are preserved verbatim.
 *
 * Errors are collected and printed in reverse order so the first mismatch
 * appears at the bottom of the terminal (most visible).
 *
 * Usage:
 *   node validate-translations.mjs
 */

import { readFile } from "fs/promises";
import { glob } from "glob";

const ORIGINAL_CHUNKS_DIR = "original-merged-chunks";
const TRANSLATED_CHUNKS_DIR = "translated-merged-chunks";

const SECTION_SEPARATOR = "--------------------";
const HEADER_SEPARATOR = "********************";

const CHOICE_MARKER_ORIG = "[選択肢 / Choices:]";
const CHOICE_MARKER_TRANS = "[Choices]";
const CHOICE_ITEM_RE = /^\d+\.\s/;

/**
 * Classify a line into a structural type with bracket-specific subtypes:
 *   "source"         — speaker name (＃ in original, $ in translated)
 *   "choice-marker"  — choice header ([選択肢 / Choices:] / [Choices])
 *   "choice-item"    — numbered choice option (1. …, 2. …)
 *   "speech-quote"   — quoted speech (「」 / \u201C\u201D / "")
 *   "speech-paren"   — thought/parenthetical (（） / ())
 *   "speech-bracket" — emphasis (【】 / [])
 *   "normal"         — narration / everything else
 */
function lineType(line, isTranslated) {
  if (isTranslated ? line.startsWith("$") : line.startsWith("＃"))
    return "source";

  if (line === CHOICE_MARKER_ORIG || (isTranslated && line === CHOICE_MARKER_TRANS))
    return "choice-marker";
  if (CHOICE_ITEM_RE.test(line)) return "choice-item";

  if (isTranslated) {
    if (line.startsWith("\u201C") && line.endsWith("\u201D")) return "speech-quote";
    if (line.startsWith('"') && line.endsWith('"')) return "speech-quote";
    if (line.startsWith("(") && line.endsWith(")")) return "speech-paren";
    if (line.startsWith("[") && line.endsWith("]")) return "speech-bracket";
  } else {
    if (line.startsWith("「") && line.endsWith("」")) return "speech-quote";
    if (line.startsWith("『") && line.endsWith("』")) return "speech-quote";
    if (line.startsWith("（") && line.endsWith("）")) return "speech-paren";
    if (line.startsWith("【") && line.endsWith("】")) return "speech-bracket";
  }

  return "normal";
}

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
 * Parse all chunk files in a directory into a Map of
 * { fileName → { lines, chunkPath, startLine } }.
 */
async function parseSectionsFromChunks(dir) {
  const chunkFiles = (await glob(`${dir}/part-*.txt`)).sort();
  const sections = new Map();

  for (const chunkPath of chunkFiles) {
    const text = await readFile(chunkPath, "utf-8");
    const allLines = text.split("\n");

    let i = 0;
    while (i < allLines.length) {
      // Scan for the next section separator.
      if (allLines[i] !== SECTION_SEPARATOR) { i++; continue; }

      const sectionStartLine = i + 1; // 1-indexed
      i++; // skip separator
      if (i >= allLines.length) break;

      const fileName = allLines[i].trim();
      i++; // skip filename
      if (i >= allLines.length || allLines[i] !== HEADER_SEPARATOR) continue;
      i++; // skip header separator

      // Collect non-empty content lines and their 1-indexed chunk line numbers.
      const contentLines = [];
      const contentLineNos = [];
      while (i < allLines.length && allLines[i] !== SECTION_SEPARATOR) {
        if (allLines[i].length > 0) {
          contentLines.push(allLines[i]);
          contentLineNos.push(i + 1);
        }
        i++;
      }

      sections.set(fileName, {
        lines: contentLines,
        lineNos: contentLineNos,
        chunkPath,
        startLine: sectionStartLine,
      });
    }
  }

  return sections;
}

async function main() {
  // Step 1: Parse sections from both chunk directories.
  const origSections = await parseSectionsFromChunks(ORIGINAL_CHUNKS_DIR);
  const transSections = await parseSectionsFromChunks(TRANSLATED_CHUNKS_DIR);

  let checked = 0;
  let mismatched = 0;
  const errors = [];

  // Step 2: Validate each original section against its translated counterpart.
  for (const [fileName, origEntry] of origSections) {
    const { lines: origLines, lineNos: origLineNos, chunkPath: origChunk, startLine: origStart } = origEntry;

    // Step 2a: Check that the translated chunks have a matching section.
    if (!transSections.has(fileName)) {
      mismatched++;
      errors.push({
        header: `✗  ${origChunk}:${origStart} > ${fileName}`,
        details: ["   Missing from translated chunks"],
      });
      continue;
    }

    checked++;
    const transEntry = transSections.get(fileName);
    const { lines: transLines, lineNos: transLineNos, chunkPath: transChunk, startLine: transStart } = transEntry;
    const sectionErrors = [];
    let firstErrorLineIdx = -1;

    if (origLines.length !== transLines.length) {
      // Step 2b: Non-empty line counts must match.
      sectionErrors.push(
        `Line count mismatch: original has ${origLines.length} lines, translated has ${transLines.length} lines`,
      );

      const minLen = Math.min(origLines.length, transLines.length);
      for (let i = 0; i < minLen; i++) {
        const origType = lineType(origLines[i], false);
        const transType = lineType(transLines[i], true);
        if (origType !== transType) {
          if (firstErrorLineIdx === -1) firstErrorLineIdx = i;
          sectionErrors.push(
            `First type mismatch at line ${i + 1} (${origType} vs. ${transType}):\n     original:   ${origLines[i]}\n     translated: ${transLines[i]}`,
          );
          break;
        }
      }
    } else {
      // Step 2c: Line-by-line structural comparison.
      for (let i = 0; i < origLines.length; i++) {
        const origLine = origLines[i];
        const transLine = transLines[i];
        const origType = lineType(origLine, false);
        const transType = lineType(transLine, true);

        if (origType !== transType) {
          if (firstErrorLineIdx === -1) firstErrorLineIdx = i;
          sectionErrors.push(
            `Line ${i + 1}: type mismatch (${origType} vs. ${transType})\n     original:   ${origLine}\n     translated: ${transLine}`,
          );
          break;
        } else if (origType === "source") {
          const origName = origLine.slice(1);
          const transName = transLine.slice(1);
          const expectedEN = SPEAKER_MAP.get(origName);

          if (!expectedEN) {
            if (firstErrorLineIdx === -1) firstErrorLineIdx = i;
            sectionErrors.push(
              `Line ${i + 1}: unknown speaker "${origName}" — add to SPEAKER_MAP`,
            );
          } else if (transName !== expectedEN) {
            if (firstErrorLineIdx === -1) firstErrorLineIdx = i;
            sectionErrors.push(
              `Line ${i + 1}: speaker name mismatch\n     expected: $${expectedEN}\n     got:      ${transLine}`,
            );
          }
        }
      }
    }

    if (sectionErrors.length > 0) {
      mismatched++;
      const origErrLine = firstErrorLineIdx >= 0 && origLineNos[firstErrorLineIdx]
        ? origLineNos[firstErrorLineIdx] : origStart;
      const transErrLine = firstErrorLineIdx >= 0 && transLineNos[firstErrorLineIdx]
        ? transLineNos[firstErrorLineIdx] : transStart;
      errors.push({
        header: `✗  ${origChunk}:${origErrLine} | ${transChunk}:${transErrLine} > ${fileName}`,
        details: sectionErrors.map((e) => `   ${e}`),
      });
    }
  }

  // Step 3: Warn about extra sections in translated that have no original.
  const extraInTranslated = [...transSections.keys()].filter(
    (f) => !origSections.has(f),
  );
  if (extraInTranslated.length > 0) {
    const details = extraInTranslated.map((f) => {
      const entry = transSections.get(f);
      return `   ${entry.chunkPath}:${entry.startLine} > ${f}`;
    });
    errors.push({
      header: "⚠  Extra sections in translated chunks not in original:",
      details,
    });
  }

  // Step 4: Print errors in reverse order (first mismatch at bottom).
  if (errors.length > 0) {
    console.log("\n--- Errors (first mismatch at bottom) ---");
    for (let i = errors.length - 1; i >= 0; i--) {
      console.log(`\n${errors[i].header}`);
      for (const d of errors[i].details) {
        console.log(d);
      }
    }
  }

  // Step 5: Print summary.
  console.log("\n— Summary —");
  console.log(`  Sections checked: ${checked}`);
  console.log(`  Mismatched:       ${mismatched}`);

  if (mismatched > 0) {
    process.exit(1);
  }
}

main().catch(console.error);
