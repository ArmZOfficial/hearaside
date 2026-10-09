// The guides: one MDX file per guide and language (en/<slug>.mdx, th/<slug>.mdx), each exporting
// `meta = { title, description, order }`. A guide missing in Thai falls back to English.
import type { ComponentType } from 'react';
import type { Lang } from '../../i18n';

export interface GuideMeta { title: string; description: string; order: number; minutes?: number }
interface GuideModule { default: ComponentType; meta: GuideMeta }

const files = import.meta.glob<GuideModule>('./*/*.mdx', { eager: true });

export interface Guide { slug: string; lang: Lang; meta: GuideMeta; Body: ComponentType }

const all: Guide[] = Object.entries(files).map(([path, mod]) => {
  const [, lang, file] = /^\.\/(en|th)\/(.+)\.mdx$/.exec(path)!;
  return { slug: file, lang: lang as Lang, meta: mod.meta, Body: mod.default };
});

export const guideSlugs = [...new Set(all.map(g => g.slug))];

export function guidesFor(lang: Lang): Guide[] {
  return guideSlugs
    .map(slug => guideFor(lang, slug)!)
    .sort((a, b) => a.meta.order - b.meta.order);
}

export function guideFor(lang: Lang, slug: string): Guide | undefined {
  return all.find(g => g.slug === slug && g.lang === lang) ?? all.find(g => g.slug === slug && g.lang === 'en');
}
