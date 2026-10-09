// Pinned shared-viewer bridge, not a registered Archify timeline renderer.
// Input is produced by analyze_stage_intervals.py inside a private delivery directory.
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
const [root, directory, palette, theme, locale] = process.argv.slice(2);
if (!root || !directory || !['light', 'dark'].includes(theme) || !['en', 'zh-CN'].includes(locale)) throw new Error('invalid bridge arguments');
const { applyTemplate } = await import(pathToFileURL(path.join(root, 'renderers/shared/utils.mjs')));
const { paletteRegistry } = await import(pathToFileURL(path.join(root, 'renderers/shared/color-palettes.mjs')));
const registry = paletteRegistry();
if (!registry.palettes[palette]) throw new Error(`unknown Archify palette: ${palette}`);
const report = JSON.parse(fs.readFileSync(path.join(directory, 'stage.report.json'), 'utf8'));
let svg = fs.readFileSync(path.join(directory, 'stage.raw.svg'), 'utf8');
if (!svg.startsWith('<svg id="gamenative-stage-diagram"')) throw new Error('unsupported domain renderer output');
const colors = {};
function map(values, token) { for (const value of values.split(' ')) colors[value] = token; }
map('#08111f', 'bg');
map('#0e1a2d #0d1a2e #0d192a #0a1525', 'panel');
map('#13223a #162944', 'lane-header');
map('#1d2b43 #24334a #355173 #152239 #1c2d45 #31507d #52657f', 'lane-stroke');
map('#f8fafc #dbe4f0 #e2e8f0 #e9f0fb', 'text');
map('#94a3b8 #8290a6 #9fb1c9 #7f8ea6 #a8bad4 #6b7f9a #cbd5e1', 'text-muted');
map('#34250f #2a2412 #3a3119', 'cloud-fill');
map('#6b4d16 #ffd38a #ffb84d', 'cloud-stroke');
map('#2a1420 #3a1d2a', 'security-fill');
map('#d8b4fe #a78bfa', 'database-stroke');
map('#39c6a3 #7ee0c0', 'backend-stroke');
map('#4f8cff #3b82f6 #22d3ee', 'frontend-stroke');
map('#ff6b7a #f472b6', 'security-stroke');
map('#64748b #26354d', 'external-stroke');
map('#a3e635', 'backend-stroke'); map('#fb923c', 'messagebus-stroke');
const variable = color => colors[color.toLowerCase()] ? `var(--${colors[color.toLowerCase()]})` : color;
// Presentation attributes and CSS only: source text, paths, timestamps and metadata stay verbatim.
svg = svg.replace(/\b(fill|stroke)="(#[0-9a-f]{6})"/gi, (_, attr, color) => `${attr}="${variable(color)}"`);
svg = svg.replace(/<style>([\s\S]*?)<\/style>/, (_, css) => '<style>' + css
  .replace(/(fill|stroke):(#[0-9a-f]{6})/gi, (_, attr, color) => `${attr}:${variable(color)}`)
  .replace(/([^{}]+)\{/g, (_, selectors) => selectors.split(',').map(s => `svg#gamenative-stage-diagram ${s.trim()}`).join(',') + '{') + '</style>');
const roleKinds = { record: 'backend', source_candidate: 'frontend', replay: 'database', auxiliary: 'external', unknown: 'cloud' };
const stageSequence = ['frontend', 'backend', 'cloud', 'database', 'security', 'frontend', 'backend', 'messagebus', 'security'];
const stageKinds = Object.fromEntries(Object.keys(report.stage_stats).map((stage, i) => [stage, stageSequence[i % stageSequence.length]]));
// Read escaped attribute strings directly, never decode/re-encode the evidence payload.
const escapeAttr = s => s.replaceAll('&', '&amp;').replaceAll('"', '&quot;').replaceAll('<', '&lt;').replaceAll('>', '&gt;');
const escapedStages = Object.fromEntries(Object.entries(stageKinds).map(([key, value]) => [escapeAttr(key), value]));
svg = svg.replace(/<rect class="interval"[^>]*>/g, tag => {
  const role = tag.match(/data-source-role="([^"]*)"/)?.[1];
  const stage = tag.match(/data-stage="([^"]*)"/)?.[1];
  const kind = roleKinds[role] || escapedStages[stage] || 'external';
  return tag.replace(/fill="[^"]*"/, `fill="var(--${kind}-fill)" stroke="var(--${kind}-stroke)" stroke-width="0.8"`).replace(/opacity="[^"]*"/, 'opacity="1"');
});
const tokens = registry.palettes[palette][theme];
const standaloneVars = Object.entries(tokens).map(([key, value]) => `--${key}:${value};`).join('');
svg = svg.replace('<style>', `<style>svg#gamenative-stage-diagram[data-static-theme="${theme}"]{${standaloneVars}}\n`);
// Inline phases/legends use shared category stroke tokens; their semantic labels remain intact.
svg = svg.replace('<svg ', `<svg data-domain="gamenative-stage-v2" data-color-palette="${palette}" `);
const resolveVariable = (_, key) => {
  if (!tokens[key]) throw new Error(`unresolved palette token: ${key}`);
  return tokens[key];
};
const standalone = svg.replace('<svg ', `<svg data-static-theme="${theme}" `)
  .replace(/<style>([\s\S]*?)<\/style>/, (_, css) => '<style>' + css.replace(/var\(--([a-z-]+)\)/g, resolveVariable) + '</style>')
  .replace(/\b(fill|stroke)="var\(--([a-z-]+)\)"/g, (_, attribute, key) => `${attribute}="${resolveVariable(null, key)}"`);
fs.writeFileSync(path.join(directory, 'stage.svg'), standalone);
const template = fs.readFileSync(path.join(root, 'assets/template.html'), 'utf8');
let html = applyTemplate(template, { title: report.title, svg, cards: '', locale, colorPalette: palette });
html = html.replace('data-theme="dark" data-preset="classic"', `data-theme="${theme}" data-preset="classic"`);
fs.writeFileSync(path.join(directory, 'stage.html'), html);
fs.writeFileSync(path.join(directory, 'archify-palettes.json'), JSON.stringify(registry, null, 2) + '\n');
