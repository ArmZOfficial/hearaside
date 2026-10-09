import { describe, expect, it } from 'vitest';
import en from '../../src/i18n/en.json';
import th from '../../src/i18n/th.json';
import { formatUserCode, maskEmail, normalizeUserCode, passwordScore } from '../../shared/schemas';
import { localPath, otherLangPath, translate } from '../../src/i18n';
import { safeNext } from '../../src/components/AuthBits';

describe('strings', () => {
  it('every English key has Thai and the same {placeholders}', () => {
    for (const [k, v] of Object.entries(en)) {
      const t = (th as Record<string, string>)[k];
      expect(t, k).toBeTruthy();
      const vars = (s: string) => [...s.matchAll(/\{(\w+)\}/g)].map(m => m[1]).sort().join(',');
      expect(vars(t), k).toBe(vars(v));
    }
    expect(Object.keys(th).length).toBe(Object.keys(en).length);
  });

  it('English copy uses the mock’s typography: curly apostrophes, no straight quotes in text', () => {
    for (const [k, v] of Object.entries(en)) {
      if (k === 'dl.checksumHint') continue;   // a PowerShell command
      expect(v, k).not.toMatch(/'/);
      expect(v, k).not.toMatch(/"/);
    }
  });

  it('fills {named} values', () => {
    expect(translate('en', 'login.fromPlugin', { plugin: 'HEARASIDE Hub', computer: 'ArmZ-PC' })).toBe('Signing in to connect HEARASIDE Hub on ArmZ-PC');
  });
});

describe('language paths', () => {
  it('Thai lives under /th', () => {
    expect(localPath('th', '/download')).toBe('/th/download');
    expect(localPath('th', '/')).toBe('/th');
    expect(localPath('en', '/download')).toBe('/download');
    expect(otherLangPath('/th/guides/obs')).toBe('/guides/obs');
    expect(otherLangPath('/')).toBe('/th');
    expect(otherLangPath('/th')).toBe('/');
  });
});

describe('codes and passwords', () => {
  it('normalises codes typed in any shape and refuses look-alikes', () => {
    expect(normalizeUserCode('kq7m 2txp')).toBe('KQ7M2TXP');
    expect(normalizeUserCode('KQ7M-2TXP')).toBe('KQ7M2TXP');
    expect(normalizeUserCode('KQ7M-2TX0')).toBeNull();   // 0 is never used
    expect(normalizeUserCode('KQ7M-2TXI')).toBeNull();   // nor I
    expect(normalizeUserCode('KQ7M2TX')).toBeNull();
    expect(formatUserCode('KQ7M2TXP')).toBe('KQ7M-2TXP');
  });

  it('password strength: under 10 characters is always weak', () => {
    expect(passwordScore('Ab1!Ab1!a')).toEqual({ score: 0, missing: 1 });
    expect(passwordScore('abcdefghij').score).toBe(1);
    expect(passwordScore('correct horse battery').score).toBe(3);
  });

  it('masks the email for screen sharing', () => {
    expect(maskEmail('armz@example.com')).toBe('a•••@example.com');
  });
});

describe('safe redirects', () => {
  it('only same-site paths', () => {
    expect(safeNext('/link?code=KQ7M-2TXP')).toBe('/link?code=KQ7M-2TXP');
    expect(safeNext('//evil.example')).toBe('/account');
    expect(safeNext('https://evil.example')).toBe('/account');
    expect(safeNext('/\\evil.example')).toBe('/account');
    expect(safeNext(null, '')).toBe('');
  });
});
