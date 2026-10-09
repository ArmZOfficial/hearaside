/// <reference types="vite/client" />

interface ImportMetaEnv {
  readonly VITE_SUPABASE_URL: string;
  readonly VITE_SUPABASE_PUBLISHABLE_KEY: string;
  readonly VITE_GOOGLE_CLIENT_ID: string;
  readonly VITE_TURNSTILE_SITE_KEY: string;
  readonly VITE_AUTH_MODE: string;
}

interface ImportMeta {
  readonly env: ImportMetaEnv;
}

declare module '*.mdx' {
  import type { ComponentType } from 'react';
  export const meta: { title: string; description: string; order: number };
  const Component: ComponentType<{ components?: Record<string, ComponentType<unknown>> }>;
  export default Component;
}
