import { test, expect, type Page } from '@playwright/test';
import AxeBuilder from '@axe-core/playwright';

declare global { interface Window { testAudioContexts: AudioContext[] } }

async function login(page: Page) {
  await page.goto('/th/login');
  await page.getByLabel('อีเมล', { exact: true }).fill('admin@example.test');
  await page.getByLabel('รหัสผ่าน', { exact: true }).fill('HearasideLocal123!');
  await page.locator('button[type=submit]').click();
  await expect(page).toHaveURL(/\/th\/account$/);
  await expect(page.locator('#f-name')).toBeVisible();
}

test('Thai validation is visible and the language switch updates the document', async ({ page }) => {
  await page.goto('/th/login');
  await page.locator('button[type=submit]').click();
  await expect(page.getByText('กรอกอีเมลให้ถูกต้อง')).toBeVisible();
  await expect(page.locator('#f-email')).toHaveAttribute('aria-invalid', 'true');
  await page.locator('.langbtn').click();
  await expect(page).toHaveURL(/\/login$/);
  await expect(page.locator('html')).toHaveAttribute('lang', 'en');
});

test('login returns directly to a requested settings section or plugin flow', async ({ page }) => {
  await page.goto('/th/login?next=%2Fth%2Faccount%2Fdevices');
  await page.getByLabel('อีเมล', { exact: true }).fill('admin@example.test');
  await page.getByLabel('รหัสผ่าน', { exact: true }).fill('HearasideLocal123!');
  await page.locator('button[type=submit]').click();
  await expect(page).toHaveURL(/\/th\/account\/devices$/);
  await expect(page.locator('#acct-sec')).toHaveText('อุปกรณ์');
  await page.goto('/th/login?next=%2Fth%2Flink');
  await expect(page).toHaveURL(/\/th\/link$/);
  await expect(page.locator('.codebox').first()).toHaveValue('');
  await page.goto('/th/verify?next=%2Fth%2Faccount%2Fdevices');
  await expect(page).toHaveURL(/\/th\/account\/devices$/);
  await page.goto('/th/verify?error=access_denied');
  await expect(page.getByRole('heading', { name: 'ยืนยันอีเมลไม่สำเร็จ' })).toBeVisible();
  await page.goto('/th/reset?next=%2Fth%2Faccount%2Fsecurity');
  await page.getByLabel('รหัสผ่าน', { exact: true }).fill('HearasideLocal123!');
  await page.locator('button[type=submit]').click();
  await expect(page).toHaveURL(/\/th\/account\/security$/);
});

test('connected devices update automatically and resync after reconnecting', async ({ page, request, context }) => {
  await login(page);
  const webToken = await page.evaluate(() => sessionStorage.getItem('hs-dev-session'));
  const headers = { Authorization: `Bearer ${webToken}` };
  await page.goto('/th/account/devices');
  await expect(page.getByText('ไม่มีปลั๊กอินที่เข้าสู่ระบบอยู่')).toBeVisible();
  const start = await request.post('/api/v1/device/start', { data: { client_info: { plugin: 'track', version: '2.0.0-rust', os: 'Windows 11', daw: 'Auto sync test', computer: 'Automatic update client' } } });
  expect(start.status()).toBe(200);
  const code = await start.json();
  expect((await request.post('/api/v1/device/approve', { headers, data: { user_code: code.user_code, allow: true } })).status()).toBe(200);
  const issued = await request.post('/api/v1/device/token', { data: { device_code: code.device_code } });
  expect(issued.status()).toBe(200);
  const tokens = await issued.json();
  await expect(page.getByText('HEARASIDE Track · Automatic update client · Auto sync test')).toBeVisible({ timeout: 20000 });
  await context.setOffline(true);
  expect((await request.delete(`/api/v1/me/devices/${tokens.device_id}`, { headers })).status()).toBe(200);
  await context.setOffline(false);
  await expect(page.getByText('ไม่มีปลั๊กอินที่เข้าสู่ระบบอยู่')).toBeVisible({ timeout: 5000 });
});

test('automatic code lookup ignores a slower response for an older code', async ({ page, request }) => {
  await login(page);
  const makeCode = async (computer: string) => {
    const response = await request.post('/api/v1/device/start', { data: { client_info: { plugin: 'hub', version: '2.0.0-rust', os: 'Windows 11', daw: '', computer } } });
    expect(response.status()).toBe(200);
    return response.json();
  };
  const oldCode = await makeCode('Older code client');
  const newCode = await makeCode('Newer code client');
  let releaseOld!: () => void;
  const delayed = new Promise<void>(resolve => { releaseOld = resolve; });
  let oldRequested = false;
  await page.route('**/api/v1/device/lookup', async route => {
    if (route.request().postDataJSON().user_code === oldCode.user_code.replace('-', '')) {
      oldRequested = true;
      await delayed;
    }
    await route.continue();
  });
  await page.goto('/th/link');
  await page.locator('.codebox').first().fill(oldCode.user_code.replace('-', ''));
  await expect.poll(() => oldRequested).toBe(true);
  await page.locator('.codebox').first().fill(newCode.user_code.replace('-', ''));
  await expect(page.getByText('Newer code client', { exact: true })).toBeVisible();
  const oldResponse = page.waitForResponse(response => response.url().endsWith('/device/lookup') && response.request().postDataJSON().user_code === oldCode.user_code.replace('-', ''));
  releaseOld();
  await oldResponse;
  await expect(page.getByText('Newer code client', { exact: true })).toBeVisible();
  await expect(page.getByText('Older code client', { exact: true })).toHaveCount(0);
  await expect(page.getByRole('button', { name: 'อนุญาต', exact: true })).toBeVisible();
});

test('admin sections fit the viewport and pass the automated accessibility scan', async ({ page }) => {
  await login(page);
  await page.locator('.acctbtn').click();
  const menu = page.getByRole('menu');
  await expect(menu.getByRole('menuitem').first()).toBeFocused();
  const menuBounds = (await menu.boundingBox())!;
  expect(menuBounds.x).toBeGreaterThanOrEqual(0);
  expect(menuBounds.x + menuBounds.width).toBeLessThanOrEqual(await page.evaluate(() => innerWidth));
  await page.keyboard.press('Escape');
  await expect(menu).not.toBeVisible();
  await expect(page.locator('.acctbtn')).toBeFocused();
  for (const route of ['/th/account', '/admin', '/admin/users', '/admin/downloads']) {
    await page.goto(route);
    await expect(page.locator(route.includes('/admin') ? '.adm-body' : '.acct-body')).toBeVisible();
    await expect(page.locator('.adm-body [role=status]').filter({ hasText: 'กำลังโหลดข้อมูล' })).toHaveCount(0);
    expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(await page.evaluate(() => innerWidth));
    const scan = await new AxeBuilder({ page }).withTags(['wcag2a', 'wcag2aa', 'wcag21aa', 'wcag22aa']).analyze();
    expect(scan.violations).toEqual([]);
  }
  await page.getByLabel('เวอร์ชัน', { exact: true }).fill('latest');
  await page.getByLabel('จำนวนก่อนเริ่มนับ', { exact: true }).fill('5');
  await expect(page.getByRole('button', { name: 'บันทึก', exact: true })).toBeDisabled();
  await expect(page.getByText('ใช้รูปแบบเวอร์ชัน เช่น 1.0.10')).toBeVisible();
  for (const count of ['7', '0']) {
    await page.getByLabel('เวอร์ชัน', { exact: true }).fill('1.0.10');
    await page.getByLabel('จำนวนก่อนเริ่มนับ', { exact: true }).fill(count);
    await page.getByRole('button', { name: 'บันทึก', exact: true }).click();
    await expect(page.getByLabel('เวอร์ชัน', { exact: true })).toHaveValue('');
    await expect(page.locator('.tbl tbody tr').filter({ hasText: '1.0.10' })).toContainText(count);
  }
});

test('a browser approves a plugin and the administrator can revoke its tokens', async ({ page, request }) => {
  await login(page);
  const start = await request.post('/api/v1/device/start', { data: { client_info: { plugin: 'hub', version: '2.0.0-rust', os: 'Windows 11', daw: 'Contract test', computer: 'Rust test client' } } });
  expect(start.status()).toBe(200);
  const code = await start.json();
  await page.goto(`/th/link?code=${encodeURIComponent(code.user_code)}`);
  await expect(page.locator('.codebox').first()).toHaveValue('');
  await page.locator('.codebox').first().fill(code.user_code.replace('-', ''));
  await expect(page.getByText('Rust test client', { exact: true })).toBeVisible();
  await page.getByRole('button', { name: 'อนุญาต', exact: true }).click();
  const issued = await request.post('/api/v1/device/token', { data: { device_code: code.device_code } });
  expect(issued.status()).toBe(200);
  const tokens = await issued.json();
  const headers = { Authorization: `Bearer ${tokens.access_token}` };
  expect((await request.get('/api/v1/me', { headers })).status()).toBe(200);
  await page.goto('/admin/users');
  await page.getByLabel('ค้นหา', { exact: true }).fill('admin@example.test');
  await page.getByRole('button', { name: 'ค้นหา', exact: true }).click();
  const row = page.locator('tbody tr').filter({ hasText: 'admin@example.test' });
  await row.getByRole('button', { name: 'ออกจากปลั๊กอิน' }).click();
  await expect(page.getByRole('dialog')).toBeVisible();
  await page.keyboard.press('Escape');
  await expect(page.getByRole('dialog')).not.toBeVisible();
  await expect(row.getByRole('button', { name: 'ออกจากปลั๊กอิน' })).toBeFocused();
  await row.getByRole('button', { name: 'ออกจากปลั๊กอิน' }).click();
  await page.getByRole('dialog').getByRole('button', { name: 'ออกจากระบบปลั๊กอินทุกเครื่อง', exact: true }).click();
  await expect(page.getByRole('dialog')).not.toBeVisible();
  expect((await request.get('/api/v1/me', { headers })).status()).toBe(401);
  expect((await request.post('/api/v1/token/refresh', { data: { refresh_token: tokens.refresh_token } })).status()).toBe(401);
});

test('failed device and link requests show an error instead of an empty account', async ({ page }) => {
  await login(page);
  for (const endpoint of ['devices', 'links', 'friends']) await page.route(`**/api/v1/me/${endpoint}`, route => route.fulfill({ status: 503, contentType: 'application/json', body: '{"error":"offline"}' }));
  for (const route of ['/th/account/devices', '/th/account/links']) {
    await page.goto(route);
    await expect(page.locator('.acct-body [role=alert]').first()).toBeVisible();
    await expect(page.locator('.acct-body').getByRole('button', { name: 'ลองใหม่' }).first()).toBeVisible();
    await expect(page.getByText('ไม่มีปลั๊กอินที่เข้าสู่ระบบอยู่')).toHaveCount(0);
    await expect(page.getByText('ยังไม่มีเพื่อนที่บันทึกไว้')).toHaveCount(0);
  }
});

test('audio stops after navigating away from the demo', async ({ page }) => {
  await page.addInitScript(() => {
    window.testAudioContexts = [];
    const Original = window.AudioContext;
    window.AudioContext = class extends Original {
      constructor(options?: AudioContextOptions) { super(options); window.testAudioContexts.push(this); }
    };
  });
  await page.goto('/');
  await page.locator('.mixdemo button.btn').first().click();
  await expect.poll(() => page.evaluate(() => window.testAudioContexts.length)).toBeGreaterThan(0);
  await expect(page.locator('.mixdemo button.btn[aria-pressed=true]')).toBeVisible();
  await page.locator('footer a').filter({ hasText: 'Guides' }).click();
  await expect(page).toHaveURL(/\/guides$/);
  await expect.poll(() => page.evaluate(() => window.testAudioContexts.every(context => context.state === 'closed'))).toBe(true);
});

test('pictures zoom and pan inside the page, then return focus on Escape', async ({ page, context }) => {
  await page.goto('/th');
  const trigger = page.locator('.hero-shot button:visible');
  await trigger.click();
  const viewer = page.getByRole('dialog', { name: 'ดูภาพขนาดเต็ม' });
  await expect(viewer).toBeVisible();
  const dialogBounds = (await viewer.boundingBox())!;
  const viewport = page.viewportSize()!;
  expect(Math.abs(dialogBounds.x + dialogBounds.width / 2 - viewport.width / 2)).toBeLessThanOrEqual(1);
  expect(Math.abs(dialogBounds.y + dialogBounds.height / 2 - viewport.height / 2)).toBeLessThanOrEqual(1);
  expect(context.pages()).toHaveLength(1);
  const initial = Number.parseInt(await viewer.locator('output').innerText());
  await viewer.getByRole('button', { name: 'ซูมเข้า', exact: true }).click();
  const increased = Number.parseInt(await viewer.locator('output').innerText());
  expect(increased).toBeGreaterThan(initial);
  await viewer.getByRole('button', { name: 'ซูมออก', exact: true }).click();
  expect(Number.parseInt(await viewer.locator('output').innerText())).toBe(initial);
  await viewer.getByRole('button', { name: 'ขนาดจริง', exact: true }).click();
  await expect(viewer.locator('output')).toHaveText('100%');
  await viewer.getByRole('button', { name: 'ซูมเข้า', exact: true }).click();
  const stage = viewer.locator('.viewer-stage');
  const bounds = (await stage.boundingBox())!;
  const before = await stage.locator('img').getAttribute('style');
  await page.mouse.move(bounds.x + bounds.width / 2, bounds.y + bounds.height / 2);
  await page.mouse.down();
  await page.mouse.move(bounds.x + bounds.width / 2 + 60, bounds.y + bounds.height / 2 + 40);
  await page.mouse.up();
  expect(await stage.locator('img').getAttribute('style')).not.toBe(before);
  await viewer.getByRole('button', { name: 'พอดีจอ', exact: true }).click();
  await expect(viewer.locator('output')).toHaveText(`${initial}%`);
  if (test.info().project.name === 'mobile') {
    const session = await context.newCDPSession(page);
    const x = bounds.x + bounds.width / 2, y = bounds.y + bounds.height / 2;
    await session.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x: x - 30, y, id: 1 }, { x: x + 30, y, id: 2 }] });
    await session.send('Input.dispatchTouchEvent', { type: 'touchMove', touchPoints: [{ x: x - 70, y, id: 1 }, { x: x + 70, y, id: 2 }] });
    await session.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
    await expect.poll(async () => Number.parseInt(await viewer.locator('output').innerText())).toBeGreaterThan(initial);
    await session.detach();
  } else {
    await page.mouse.move(bounds.x + bounds.width / 2, bounds.y + bounds.height / 2);
    await page.mouse.wheel(0, -200);
    await expect.poll(async () => Number.parseInt(await viewer.locator('output').innerText())).toBeGreaterThan(initial);
  }
  const scan = await new AxeBuilder({ page }).withTags(['wcag2a', 'wcag2aa', 'wcag21aa', 'wcag22aa']).analyze();
  expect(scan.violations).toEqual([]);
  await page.keyboard.press('Escape');
  await expect(viewer).not.toBeVisible();
  await expect(trigger).toBeFocused();
  await expect(page.locator('body')).not.toHaveCSS('overflow', 'hidden');
  await page.locator('.footer-top').click();
  await expect.poll(() => page.evaluate(() => scrollY)).toBe(0);
});

test('each guide has a working illustrated example', async ({ page }) => {
  for (const slug of ['getting-started', 'obs', 'share', 'record-app-audio', 'troubleshooting']) {
    await page.goto(`/th/guides/${slug}`);
    const picture = page.locator('.guide-figure button:visible').first();
    await expect(picture).toBeVisible();
    await expect.poll(() => picture.locator('img').evaluate(image => (image as HTMLImageElement).naturalWidth)).toBeGreaterThan(0);
    await picture.click();
    await expect(page.getByRole('dialog', { name: 'ดูภาพขนาดเต็ม' })).toBeVisible();
    await page.getByRole('button', { name: 'ปิดภาพ', exact: true }).click();
    await expect(picture).toBeFocused();
  }
});

test('hero tilt animates with the mouse and removes the transform at rest', async ({ page }) => {
  test.skip(test.info().project.name === 'mobile', 'Mouse hover animation is tested on desktop.');
  await page.emulateMedia({ reducedMotion: 'no-preference' });
  await page.goto('/');
  const frame = page.locator('.hero-shot .tilt-frame');
  const content = frame.locator('.tilt-content');
  const bounds = (await frame.boundingBox())!;
  await frame.hover({ position: { x: bounds.width * .8, y: bounds.height * .2 } });
  await expect(content).toHaveCSS('transform', /matrix3d/);
  await page.mouse.move(0, 0);
  await expect(content).toHaveCSS('transform', 'none');
  await page.emulateMedia({ reducedMotion: 'reduce' });
  await page.reload();
  await frame.hover({ position: { x: bounds.width * .8, y: bounds.height * .2 } });
  await expect(content).toHaveCSS('transform', 'none');
});
