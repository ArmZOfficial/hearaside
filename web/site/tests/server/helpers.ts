import request from 'supertest';
import { createApp } from '../../server/app';
import { MemoryStore } from '../../server/lib/memoryStore';

export const CLIENT = { plugin: 'hub', version: '0.9.0', os: 'Windows 11', daw: 'Studio One', computer: 'Test-PC' } as const;

/** a fresh API with one signed-in website user */
export async function setup() {
  const store = new MemoryStore();
  const app = createApp(store);
  const api = request(app);
  store.signUp('singer@example.test', 'correct horse battery', 'Singer', 'en');
  const web = store.signIn('singer@example.test', 'correct horse battery')!;
  return { store, app, api, web, auth: { Authorization: `Bearer ${web}` } };
}

/** runs the whole device flow and returns the plug-in's tokens */
export async function signInPlugin(ctx: Awaited<ReturnType<typeof setup>>) {
  const start = await ctx.api.post('/api/v1/device/start').send({ client_info: CLIENT });
  await ctx.api.post('/api/v1/device/approve').set(ctx.auth).send({ user_code: start.body.user_code, allow: true });
  const tok = await ctx.api.post('/api/v1/device/token').send({ device_code: start.body.device_code });
  return { access: tok.body.access_token as string, refresh: tok.body.refresh_token as string, deviceId: tok.body.device_id as string };
}
