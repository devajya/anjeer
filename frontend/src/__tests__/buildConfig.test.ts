import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

const repoRoot = resolve(__dirname, '../../../');

describe('lighthouserc.json', () => {
  it('is valid JSON', () => {
    const raw = readFileSync(resolve(repoRoot, 'lighthouserc.json'), 'utf-8');
    expect(() => JSON.parse(raw)).not.toThrow();
  });

  it('contains a performance assertion', () => {
    const config = JSON.parse(
      readFileSync(resolve(repoRoot, 'lighthouserc.json'), 'utf-8'),
    );
    expect(config.ci.assert.assertions['categories:performance']).toBeDefined();
    const [level, opts] = config.ci.assert.assertions['categories:performance'];
    expect(level).toBe('error');
    expect(opts.minScore).toBeGreaterThanOrEqual(0.85);
  });
});

describe('Makefile', () => {
  it('contains lighthouse: target', () => {
    const makefile = readFileSync(resolve(repoRoot, 'Makefile'), 'utf-8');
    expect(makefile).toMatch(/^lighthouse:/m);
  });
});
