#!/usr/bin/env node
// Generates a tiny deterministic PDF without third-party packages.

import { writeFile } from "node:fs/promises";
import process from "node:process";

const output = process.argv[2];
if (!output) {
  console.error("Usage: node make-pdfrender-smoke-pdf.mjs output.pdf");
  process.exit(2);
}

const stream = [
  "0.93 0.95 1 rg 36 690 540 80 re f",
  "0.1 0.3 0.7 RG 3 w 36 690 540 80 re S",
  "BT /F1 28 Tf 0.1 0.1 0.1 rg 60 720 Td (pdfrender smoke test) Tj ET",
  "BT /F2 16 Tf 0.2 0.2 0.2 rg 60 650 Td (Serif variant and reusable HTML text) Tj ET",
  "BT /F3 40 Tf 450 600 Td (A) Tj ET",
  "q 80 0 0 80 60 530 cm BI /W 2 /H 2 /CS /RGB /BPC 8 /F /AHx ID FF000000FF00000FFFFFFFFF> EI Q",
].join("\n");
const type3Glyph =
  "1000 0 0 0 1000 1000 d1 0.9 0.2 0.1 rg 0 0 m 500 1000 l 1000 0 l h f";
const objects = [
  "<< /Type /Catalog /Pages 2 0 R >>",
  "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
  "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R /F2 6 0 R /F3 7 0 R >> >> /Contents 4 0 R >>",
  `<< /Length ${Buffer.byteLength(stream)} >>\nstream\n${stream}\nendstream`,
  "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
  "<< /Type /Font /Subtype /Type1 /BaseFont /Times-Roman /Encoding /WinAnsiEncoding >>",
  "<< /Type /Font /Subtype /Type3 /FontBBox [0 0 1000 1000] /FontMatrix [0.001 0 0 0.001 0 0] /CharProcs << /A 8 0 R >> /Encoding << /Type /Encoding /Differences [65 /A] >> /FirstChar 65 /LastChar 65 /Widths [1000] /Resources << >> >>",
  `<< /Length ${Buffer.byteLength(type3Glyph)} >>\nstream\n${type3Glyph}\nendstream`,
];

let pdf = "%PDF-1.4\n%pdfrender\n";
const offsets = [0];
for (let i = 0; i < objects.length; ++i) {
  offsets.push(Buffer.byteLength(pdf));
  pdf += `${i + 1} 0 obj\n${objects[i]}\nendobj\n`;
}
const xrefOffset = Buffer.byteLength(pdf);
pdf += `xref\n0 ${objects.length + 1}\n`;
pdf += "0000000000 65535 f \n";
for (const offset of offsets.slice(1)) {
  pdf += `${String(offset).padStart(10, "0")} 00000 n \n`;
}
pdf += `trailer\n<< /Size ${objects.length + 1} /Root 1 0 R >>\nstartxref\n${xrefOffset}\n%%EOF\n`;

await writeFile(output, pdf);
