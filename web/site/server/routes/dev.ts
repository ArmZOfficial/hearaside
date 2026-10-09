// Development-only sign-in for STORE=memory (the website in `npm run dev`, tests, the plug-in mock
// server). Mounted only when the memory store is in use; never on the real site.
import { Router } from 'express';
import { z } from 'zod';
import { email, password } from '../../shared/schemas.js';
import type { MemoryStore } from '../lib/memoryStore.js';
import { body } from '../lib/http.js';
import { limit } from '../lib/limit.js';

export function devRoutes(store: MemoryStore) {
  const r = Router();
  const tokenOf = (h?: string) => (h ?? '').replace(/^Bearer\s+/, '');
  const userOf = (h?: string) => store.sessions.get(tokenOf(h)) ?? null;

  r.post('/dev/auth/signup', limit('dev-signup', 30), (req, res) => {
    const b = body(z.object({ email, password, displayName: z.string().max(40), locale: z.enum(['en', 'th']).default('en') }).passthrough(), req, res);
    if (!b) return;
    store.signUp(b.email, b.password, b.displayName, b.locale);
    res.json({ ok: true });
  });
  r.post('/dev/auth/login', limit('dev-login', 30), (req, res) => {
    const b = body(z.object({ email: z.string(), password: z.string() }), req, res);
    if (!b) return;
    const token = store.signIn(b.email, b.password);
    token ? res.json({ token }) : res.status(401).json({ error: 'wrong' });
  });
  r.post('/dev/auth/logout', (req, res) => { store.signOut(tokenOf(req.headers.authorization)); res.json({ ok: true }); });
  r.post('/dev/auth/password', (req, res) => {
    const id = userOf(req.headers.authorization);
    const b = body(z.object({ password }), req, res);
    if (!b) return;
    if (!id) return res.status(401).json({ error: 'sign_in' });
    store.setPassword(id, b.password);
    res.json({ ok: true });
  });
  r.post('/dev/auth/email', (req, res) => {
    const id = userOf(req.headers.authorization);
    const b = body(z.object({ email }), req, res);
    if (!b) return;
    if (!id) return res.status(401).json({ error: 'sign_in' });
    store.setEmail(id, b.email);
    res.json({ ok: true });
  });
  r.get('/dev/avatars/:user/:file', (req, res) => {
    const img = store.avatars.get(`${req.params.user}/${req.params.file}`);
    if (!img) return res.status(404).end();
    res.type('image/webp').send(img);
  });
  return r;
}
