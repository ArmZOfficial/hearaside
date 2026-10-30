import { afterEach, describe, expect, it, vi } from 'vitest';
import { over } from '../../server/lib/limit';

afterEach(() => vi.useRealTimers());

describe('minute and day limits', () => {
  it('keeps numeric callers working and counts the request at the boundary', async () => {
    expect(await over('numeric', 'one', 2)).toBe(false);
    expect(await over('numeric', 'one', 2)).toBe(false);
    expect(await over('numeric', 'one', 2)).toBe(true);
    expect(await over('numeric', 'two', 2)).toBe(false);
  });

  it('enforces the day limit across minute windows and resets the next UTC day', async () => {
    vi.useFakeTimers();
    vi.setSystemTime(new Date('2026-10-30T00:00:00Z'));
    const limits = { perMinute: 30, perDay: 500 };
    for (let i = 0; i < 501; i++) {
      vi.setSystemTime(new Date(Date.parse('2026-10-30T00:00:00Z') + Math.floor(i / 30) * 60000));
      expect(await over('turn', 'one', limits)).toBe(i === 500);
    }
    vi.setSystemTime(new Date('2026-10-31T00:00:00Z'));
    expect(await over('turn', 'one', limits)).toBe(false);
  });
});
