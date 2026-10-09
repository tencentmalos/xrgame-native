import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { pathToFileURL, fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';
const root = process.env.ARCHIFY_ROOT;
const input = process.env.GAMENATIVE_STAGE_TEST_BUNDLE || path.join(path.dirname(fileURLToPath(import.meta.url)), 'fixtures/synthetic-six-frame.stage.json');
test('domain viewer preserves intervals, pin interaction and exported colors across all palette modes', {
  skip: !root || !input || !process.env.ARCHIFY_CHROME ? 'Set ARCHIFY_ROOT and ARCHIFY_CHROME; GAMENATIVE_STAGE_TEST_BUNDLE may override the synthetic fixture.' : false,
}, async () => {
  const { ChromeVisualBrowser, findChrome } = await import(pathToFileURL(path.join(root, 'bin/visual-check.mjs')));
  const { PALETTES } = await import(pathToFileURL(path.join(root, 'renderers/shared/color-palettes.mjs')));
  const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'gamenative-stage-browser-'));
  const browser = new ChromeVisualBrowser(findChrome());
  try {
    const output = path.join(temp, 'run');
    const script = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../scripts/deliver_stage_diagram.py');
    execFileSync(process.env.PYTHON3 || 'python3', [script, '--input', input, '--output-dir', output, '--archify', root, '--evidence-kind', 'synthetic']);
    await browser.inspect({ artifactPath: path.join(output, 'stage.html'), width: 1440, height: 900, theme: 'dark' });
    const session = await browser.sessionPromise;
    const result = await browser.cdp.send('Runtime.evaluate', { expression: `(async () => {
      const geometry = () => Array.from(document.querySelectorAll('rect.interval')).map(e => [e.getAttribute('x'), e.getAttribute('width'), e.dataset.token, e.dataset.source, e.dataset.frame]);
      const initial = geometry(); const results = [];
      HTMLAnchorElement.prototype.click = function () {};
      let captured; const create = URL.createObjectURL;
      URL.createObjectURL = function(blob) { if (blob.type.includes('svg')) captured = blob.text(); return create.call(URL, blob); };
      for (const palette of ['studio','ocean','sunset','feishu']) for (const theme of ['light','dark']) {
        Archify.palette.apply(palette);
        if (document.documentElement.dataset.theme !== theme) Archify.theme.toggle();
        const interval = document.querySelector('rect.interval');
        interval.dispatchEvent(new KeyboardEvent('keydown', { key:'Enter', bubbles:true }));
        const pinned = interval.classList.contains('selected') && document.getElementById('hover-card').style.display !== 'none';
        const detail = document.getElementById('selection-detail').textContent;
        document.dispatchEvent(new KeyboardEvent('keydown', { key:'Escape', bubbles:true }));
        const cleared = !interval.classList.contains('selected');
        await Archify.exportMenu.run('svg');
        const svg = await captured;
        const parsed = new DOMParser().parseFromString(svg, 'image/svg+xml');
        results.push({ palette, theme, initial, geometry:geometry(), pinned, cleared, detail,
          background:getComputedStyle(document.querySelector('#gamenative-stage-diagram > rect')).fill,
          text:getComputedStyle(document.querySelector('#gamenative-stage-diagram .title')).fill,
          exported:Array.from(parsed.querySelectorAll('rect.interval')).map(e => [e.getAttribute('x'),e.getAttribute('width'),e.getAttribute('data-token'),e.getAttribute('data-source'),e.getAttribute('data-frame')]),
          svg, parseError:!!parsed.querySelector('parsererror') });
      }
      return results;
    })()`, returnByValue: true, awaitPromise: true }, session);
    assert.equal(result.exceptionDetails, undefined);
    for (const record of result.result.value) {
      assert.deepEqual(record.geometry, record.initial);
      assert.deepEqual(record.exported, record.initial);
      assert.ok(record.pinned && record.cleared, `${record.palette}/${record.theme}: pin/escape`);
      assert.ok(record.detail.includes('source frame'));
      assert.equal(record.parseError, false);
      const rgb = hex => 'rgb(' + hex.match(/\w{2}/g).map(v => parseInt(v,16)).join(', ') + ')';
      assert.equal(record.text, rgb(PALETTES[record.palette][record.theme].text));
      assert.equal(record.background, rgb(PALETTES[record.palette][record.theme].bg));
      assert.ok(record.svg.includes(PALETTES[record.palette].light.bg));
      assert.ok(record.svg.includes(PALETTES[record.palette].dark.bg));
    }
  } finally { await browser.close(); fs.rmSync(temp, { recursive: true, force: true }); }
});
