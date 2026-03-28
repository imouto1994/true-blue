/**
 * Extract Choice Groups with Translations
 *
 * Parses all original merged chunks to extract player choice groups,
 * then pairs each choice with its translated counterpart from the
 * line-aligned translated merged chunks.
 *
 * Detection pattern (original chunks):
 *   - Choice marker: `[選択肢 / Choices:]`
 *   - Choice items:  `N. 　{text}` (numbered lines immediately after marker)
 *
 * Translated chunks use half-width equivalents:
 *   - Choice marker: `[Choices]`
 *   - Choice items:  `N. {text}`
 *
 * Output: `choices-with-translations.txt`
 *
 * Usage:
 *   node extract-choices.mjs
 */

import { readFile, writeFile } from "fs/promises";
import { glob } from "glob";

const ORIGINAL_CHUNKS_DIR = "original-merged-chunks";
const TRANSLATED_CHUNKS_DIR = "translated-merged-chunks";
const OUTPUT_FILE = "choices-with-translations.txt";

const SECTION_SEPARATOR = "--------------------";
const HEADER_SEPARATOR = "********************";

const CHOICE_MARKER = "[選択肢 / Choices:]";
const CHOICE_ITEM_RE = /^(\d+)\.\s/;

/**
 * Parse all chunk files in a directory into a Map of
 * { sectionName → contentLines[] }.
 * Preserves all lines so indices stay aligned between original and translated.
 */
async function parseSectionsFromChunks(dir) {
  const chunkFiles = (await glob(`${dir}/part-*.txt`)).sort();
  const sections = new Map();

  for (const chunkPath of chunkFiles) {
    const text = await readFile(chunkPath, "utf-8");
    const allLines = text.split("\n");

    let i = 0;
    while (i < allLines.length) {
      if (allLines[i] !== SECTION_SEPARATOR) { i++; continue; }
      i++;
      if (i >= allLines.length) break;

      const sectionName = allLines[i].trim();
      i++;
      if (i >= allLines.length || allLines[i] !== HEADER_SEPARATOR) continue;
      i++;

      const contentLines = [];
      while (i < allLines.length && allLines[i] !== SECTION_SEPARATOR) {
        contentLines.push(allLines[i]);
        i++;
      }

      sections.set(sectionName, contentLines);
    }
  }

  return sections;
}

/**
 * Extract choice groups from a section's content lines.
 * Returns an array of groups, each group being an array of
 * { index, text, lineIdx } objects.
 */
function extractChoiceGroups(lines) {
  const groups = [];

  for (let i = 0; i < lines.length; i++) {
    if (lines[i] !== CHOICE_MARKER) continue;

    const group = [];
    for (let j = i + 1; j < lines.length; j++) {
      const match = lines[j].match(CHOICE_ITEM_RE);
      if (!match) break;
      group.push({
        index: parseInt(match[1]),
        text: lines[j],
        lineIdx: j,
      });
    }

    if (group.length > 0) {
      groups.push(group);
    }
  }

  return groups;
}

async function main() {
  const origSections = await parseSectionsFromChunks(ORIGINAL_CHUNKS_DIR);
  const transSections = await parseSectionsFromChunks(TRANSLATED_CHUNKS_DIR);

  const outputSections = [];
  let totalGroups = 0;
  let totalChoices = 0;
  let untranslated = 0;

  for (const [sectionName, origLines] of origSections) {
    const groups = extractChoiceGroups(origLines);
    if (groups.length === 0) continue;

    const transLines = transSections.get(sectionName);
    const lines = [];

    for (let g = 0; g < groups.length; g++) {
      if (g > 0) lines.push("");
      lines.push(`Group ${g + 1}`);

      for (const choice of groups[g]) {
        totalChoices++;
        const transLine = transLines?.[choice.lineIdx];
        if (transLine && transLine !== choice.text) {
          lines.push(`${choice.text} → ${transLine}`);
        } else if (transLine === choice.text) {
          lines.push(`${choice.text} → [UNTRANSLATED]`);
          untranslated++;
        } else {
          lines.push(`${choice.text} → [UNTRANSLATED]`);
          untranslated++;
        }
      }
      totalGroups++;
    }

    outputSections.push(
      `${sectionName}\n${HEADER_SEPARATOR}\n${lines.join("\n")}`,
    );
  }

  const output = outputSections
    .map((s) => `${SECTION_SEPARATOR}\n${s}`)
    .join("\n");
  await writeFile(OUTPUT_FILE, output + "\n", "utf-8");

  console.log("— Summary —");
  console.log(`  Sections with choices: ${outputSections.length}`);
  console.log(`  Total groups:          ${totalGroups}`);
  console.log(`  Total choices:         ${totalChoices}`);
  console.log(`  Untranslated:          ${untranslated}`);
  console.log(`  Exported to:           ${OUTPUT_FILE}`);
}

main().catch(console.error);
