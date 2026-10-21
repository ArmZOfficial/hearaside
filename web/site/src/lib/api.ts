// The account API (/api/v1, same domain). TanStack Query keeps the answers; every call carries the
// website session token. Nothing here is needed for the marketing pages.
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import type { Device, Me, PendingDevice, ProfilePatch, SavedFriend, ShareLink } from '../../shared/schemas';
import { useAuth } from './auth';

export class ApiError extends Error {
  constructor(public status: number, public code: string) {
    super(code);
  }
}

export async function call<T>(token: string | null, method: string, path: string, body?: unknown): Promise<T> {
  let res: Response;
  try {
    res = await fetch(`/api/v1${path}`, {
      method,
      headers: {
        ...(body !== undefined && !(body instanceof FormData) ? { 'Content-Type': 'application/json' } : {}),
        ...(token ? { Authorization: `Bearer ${token}` } : {}),
      },
      body: body === undefined ? undefined : body instanceof FormData ? body : JSON.stringify(body),
    });
  } catch {
    throw new ApiError(0, 'offline');
  }
  const json = await res.json().catch(() => ({}));
  if (!res.ok) throw new ApiError(res.status, (json as { error?: string }).error ?? 'failed');
  return json as T;
}

export function useApi() {
  const { token } = useAuth();
  return <T,>(method: string, path: string, body?: unknown) => call<T>(token, method, path, body);
}

export function useMe() {
  const { token, status } = useAuth();
  return useQuery({
    queryKey: ['me', token],
    queryFn: () => call<Me>(token, 'GET', '/me'),
    enabled: status === 'signedIn' && !!token,
    staleTime: 30_000,
    retry: (n, e) => n < 2 && !(e instanceof ApiError && e.status === 401),
  });
}

export function useUpdateMe() {
  const api = useApi();
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (patch: ProfilePatch) => api<Me>('PATCH', '/me', patch),
    onSuccess: me => qc.setQueriesData({ queryKey: ['me'] }, me),
  });
}

export function useDevices() {
  const { token, status } = useAuth();
  return useQuery({
    queryKey: ['devices', token],
    queryFn: () => call<{ devices: Device[] }>(token, 'GET', '/me/devices'),
    enabled: status === 'signedIn' && !!token,
  });
}

export function useLinks() {
  const { token, status } = useAuth();
  return useQuery({
    queryKey: ['links', token],
    queryFn: () => call<{ links: ShareLink[] }>(token, 'GET', '/me/links'),
    enabled: status === 'signedIn' && !!token,
  });
}

export function useFriends() {
  const { token, status } = useAuth();
  return useQuery({
    queryKey: ['friends', token],
    queryFn: () => call<{ friends: SavedFriend[] }>(token, 'GET', '/me/friends'),
    enabled: status === 'signedIn' && !!token,
  });
}

// --- back office (only answers for admins; everyone else gets 404) ---------------------------------

export interface AdminVersion { version: string; site: number; github: number; manual: number; total: number }
export interface AdminDownloads { total: number; counted: number; before: number; last30: number; versions: AdminVersion[]; daily: { day: string; count: number }[] }
export interface AdminOverview {
  counts: { users: number; deleted: number; devices: number; activeDevices7d: number; links: number; signups: { day: string; count: number }[] };
  downloads: Pick<AdminDownloads, 'total' | 'last30' | 'daily' | 'versions'>;
}
export interface AdminUser {
  id: string; email: string; handle: string; displayName: string; createdAt: string; lastSeenAt: string | null;
  devices: number; links: number; deletedAt: string | null;
}

export function useIsAdmin() {
  const { token, status } = useAuth();
  return useQuery({
    queryKey: ['admin-me', token],
    queryFn: () => call<{ admin: true }>(token, 'GET', '/admin/me').then(() => true, () => false),
    enabled: status === 'signedIn' && !!token,
    staleTime: 5 * 60_000,
  }).data === true;
}

export function useAdminQuery<T>(key: string, path: string) {
  const { token, status } = useAuth();
  return useQuery({ queryKey: ['admin', key, path, token], queryFn: () => call<T>(token, 'GET', path), enabled: status === 'signedIn' && !!token });
}

export type { PendingDevice };
