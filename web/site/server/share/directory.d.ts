// Types for directory.js (plain JS, copied from web/share-vercel/lib by its sync script).
export interface DirStore {
  get(key: string): Promise<string | null>;
  set(key: string, value: string, ttlSec: number): Promise<void>;
  del(key: string): Promise<void>;
  incr(key: string, ttlSec: number): Promise<number>;
}
export interface Result { status: number; body: Record<string, unknown> }
export const LIMITS: { register: number; resolve: number };
export function sha256(s: string): string;
export function register(store: DirStore, body: unknown, now?: number): Promise<Result>;
export function unregister(store: DirStore, body: unknown): Promise<Result>;
export function resolve(store: DirStore, token: string): Promise<Result>;
export function revokeHash(store: DirStore, tokenHash: string): Promise<void>;
export function memoryStore(): DirStore;
