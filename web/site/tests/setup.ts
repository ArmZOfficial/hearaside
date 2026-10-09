import { afterEach } from 'vitest';
import { resetLimits } from '../server/lib/limit';

// each test starts with fresh rate-limit counters
afterEach(() => resetLimits());

if (typeof window !== 'undefined') {
  await import('@testing-library/jest-dom/vitest');
  const { cleanup } = await import('@testing-library/react');
  afterEach(() => cleanup());
}
