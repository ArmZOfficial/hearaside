import type { DirStore } from './directory.js';
export function redisFromEnv(env?: Record<string, string | undefined>): DirStore | null;
export function store(): DirStore;
