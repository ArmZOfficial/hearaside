import { defineConfig } from '@playwright/test';

export default defineConfig({
  testDir: './tests/e2e',
  timeout: 45000,
  expect: { timeout: 15000 },
  workers: 1,
  use: { baseURL: 'http://localhost:5173', reducedMotion: 'reduce', trace: 'retain-on-failure', screenshot: 'only-on-failure' },
  projects: [
    { name: 'desktop', use: { browserName: 'chromium', channel: process.platform === 'win32' ? 'msedge' : undefined, viewport: { width: 1440, height: 1000 }, colorScheme: 'light' } },
    { name: 'mobile', use: { browserName: 'chromium', channel: process.platform === 'win32' ? 'msedge' : undefined, viewport: { width: 320, height: 844 }, colorScheme: 'dark', isMobile: true, hasTouch: true, deviceScaleFactor: 2 } },
  ],
  webServer: { command: 'npm run dev:local', url: 'http://localhost:5173/api/v1/health', reuseExistingServer: !process.env.CI, timeout: 30000 },
});
