// "/privacy" and "/terms" (A7): drafts in MDX, EN + TH, to be reviewed before accounts open.
import type { ComponentType } from 'react';
import { useT } from '../i18n';
import { PageShell } from '../components/Layout';
import { Seo } from '../lib/seo';
import PrivacyEn from '../content/legal/en/privacy.mdx';
import PrivacyTh from '../content/legal/th/privacy.mdx';
import TermsEn from '../content/legal/en/terms.mdx';
import TermsTh from '../content/legal/th/terms.mdx';

const DOCS: Record<'privacy' | 'terms', Record<'en' | 'th', ComponentType>> = {
  privacy: { en: PrivacyEn, th: PrivacyTh },
  terms: { en: TermsEn, th: TermsTh },
};

export default function Legal({ kind }: { kind: 'privacy' | 'terms' }) {
  const { t, lang } = useT();
  const Body = DOCS[kind][lang];
  return (
    <PageShell>
      <Seo title={`${t(kind === 'privacy' ? 'privacy.h1' : 'terms.h1')} · HEARASIDE`} />
      <div className="wrap guide-wrap pt-10">
        <article className="card guide-body prose-hs"><Body /></article>
      </div>
    </PageShell>
  );
}
