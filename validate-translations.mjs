/**
 * Validate Translations
 *
 * Compares `merged-translated.txt` against `merged-original.txt` to ensure
 * structural consistency across all file sections.
 *
 * Checks performed per section:
 *   1. Every original section has a matching translated section (by filename).
 *   2. Non-empty line counts match.
 *   3. Line types match (source / speech / normal).
 *   4. Speech source names match via SPEAKER_MAP (JP → EN).
 *
 * Original lines use full-width ＃ for speech source and 「」 for speech
 * content. Translated lines use half-width # for speech source and \u201C\u201D
 * for speech content.
 *
 * Usage:
 *   node validate-translations.mjs
 */

import { readFile } from "fs/promises";

const ORIGINAL_FILE = "merged-original.txt";
const TRANSLATED_FILE = "merged-translated.txt";

const SECTION_SEPARATOR = "--------------------";
const HEADER_SEPARATOR = "********************";

// Original uses full-width ＃, translated uses half-width #.
const isSpeechSourceJP = (line) => line.startsWith("＃");
const isSpeechSourceEN = (line) => line.startsWith("#");

// Original speech content uses 「」 brackets.
const isSpeechContentJP = (line) =>
  line.startsWith("「") && line.endsWith("」");

// Translated speech content uses \u201C\u201D curly quotes.
const isSpeechContentEN = (line) =>
  line.startsWith("\u201C") && line.endsWith("\u201D");

/**
 * Classify a line into one of three structural types:
 *   "source"  — speaker name (＃ in original, # in translated)
 *   "speech"  — speech content (「…」 in original, \u201C…\u201D in translated)
 *   "normal"  — narration / everything else
 */
function lineType(line, isTranslated) {
  if (isTranslated ? isSpeechSourceEN(line) : isSpeechSourceJP(line))
    return "source";
  if (isTranslated ? isSpeechContentEN(line) : isSpeechContentJP(line))
    return "speech";
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
 * Parse a merged text file into a Map of { fileName → nonEmptyLines[] }.
 */
function parseSections(text) {
  // Step 1: Split file into raw blocks by the section separator line.
  const raw = text.split(`${SECTION_SEPARATOR}\n`);
  const sections = new Map();

  for (const block of raw) {
    // Step 2: Locate the header separator to split filename from body.
    const headerEnd = block.indexOf(`\n${HEADER_SEPARATOR}\n`);
    if (headerEnd === -1) continue;

    const fileName = block.slice(0, headerEnd).trim();
    const body = block.slice(headerEnd + HEADER_SEPARATOR.length + 2);

    // Step 3: Keep only non-empty lines (empty lines are ignored per spec).
    const lines = body.split("\n").filter((l) => l.length > 0);
    sections.set(fileName, lines);
  }

  return sections;
}

async function main() {
  // Step 1: Read both merged files.
  const originalText = await readFile(ORIGINAL_FILE, "utf-8");
  const translatedText = await readFile(TRANSLATED_FILE, "utf-8");

  // Step 2: Parse into section maps keyed by filename.
  const origSections = parseSections(originalText);
  const transSections = parseSections(translatedText);

  let checked = 0;
  let mismatched = 0;

  // Step 3: Validate each original section against its translated counterpart.
  for (const [fileName, origLines] of origSections) {
    // Step 3a: Check that the translated file has a matching section.
    if (!transSections.has(fileName)) {
      console.log(`\n✗  ${fileName}`);
      console.log("   Missing from translated file");
      mismatched++;
      continue;
    }

    checked++;
    const transLines = transSections.get(fileName);
    const sectionErrors = [];

    if (origLines.length !== transLines.length) {
      // Step 3b: Non-empty line counts must match.
      sectionErrors.push(
        `Line count mismatch: original has ${origLines.length} lines, translated has ${transLines.length} lines`,
      );

      // Report the first line where the type diverges to aid debugging.
      const minLen = Math.min(origLines.length, transLines.length);
      for (let i = 0; i < minLen; i++) {
        const origType = lineType(origLines[i], false);
        const transType = lineType(transLines[i], true);
        if (origType !== transType) {
          sectionErrors.push(
            `First type mismatch at line ${i + 1} (${origType} vs. ${transType}):\n     original:   ${origLines[i]}\n     translated: ${transLines[i]}`,
          );
          break;
        }
      }
    } else {
      // Step 3c: Line-by-line structural comparison.
      for (let i = 0; i < origLines.length; i++) {
        const origLine = origLines[i];
        const transLine = transLines[i];
        const origType = lineType(origLine, false);
        const transType = lineType(transLine, true);

        if (origType !== transType) {
          sectionErrors.push(
            `Line ${i + 1}: type mismatch (${origType} vs. ${transType})\n     original:   ${origLine}\n     translated: ${transLine}`,
          );
        } else if (origType === "source") {
          // Step 3d: Verify speaker name maps correctly (JP ＃ → EN #).
          const origName = origLine.slice(1);
          const transName = transLine.slice(1);
          const expectedEN = SPEAKER_MAP.get(origName);

          if (!expectedEN) {
            sectionErrors.push(
              `Line ${i + 1}: unknown speaker "${origName}" — add to SPEAKER_MAP`,
            );
          } else if (transName !== expectedEN) {
            sectionErrors.push(
              `Line ${i + 1}: speaker name mismatch\n     expected: #${expectedEN}\n     got:      ${transLine}`,
            );
          }
        }
      }
    }

    if (sectionErrors.length > 0) {
      mismatched++;
      console.log(`\n✗  ${fileName}`);
      for (const err of sectionErrors) {
        console.log(`   ${err}`);
      }
    }
  }

  // Step 4: Warn about extra sections in translated that have no original.
  const extraInTranslated = [...transSections.keys()].filter(
    (f) => !origSections.has(f),
  );
  if (extraInTranslated.length > 0) {
    console.log(`\n⚠  Extra sections in translated file not in original:`);
    for (const f of extraInTranslated) {
      console.log(`   ${f}`);
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
