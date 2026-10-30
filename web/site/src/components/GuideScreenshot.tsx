import { useT } from '../i18n';
import { Screenshot } from './ui/Kit';

export function GuideScreenshot({ name, caption }: { name: 'hub' | 'track' | 'share' | 'app' | 'setup'; caption: string }) {
  const { lang, t } = useT();
  const width = name === 'track' ? 380 : name === 'app' ? 440 : 1040;
  const height = name === 'track' ? 640 : name === 'app' ? 600 : 790;
  return <figure className={`guide-figure ${name === 'track' || name === 'app' ? 'narrow' : ''}`}>
    <Screenshot name={name} lang={lang} alt={caption} width={width} height={height} />
    <figcaption>{caption}<span>{t('guides.enlarge')}</span></figcaption>
  </figure>;
}
