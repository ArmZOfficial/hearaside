/** Only local paths can survive an authentication round trip. */
export function safeNext(raw: string | null | undefined, fallback = '/account'): string {
  if (!raw || !raw.startsWith('/') || raw.startsWith('//') || /[\u0000-\u0020\\]/.test(raw)) return fallback;
  return raw;
}

export function authCallbackPath(kind: 'verify' | 'reset', locale: string, next?: string): string {
  const prefix = locale === 'th' ? '/th' : '';
  return `${prefix}/${kind}?${new URLSearchParams({ next: safeNext(next, `${prefix}/account`) })}`;
}
