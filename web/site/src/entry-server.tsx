// Prerender entry (built with `vite build --ssr`): renders one URL to HTML, waiting for lazy pages.
import { prerenderToNodeStream } from 'react-dom/static';
import { StaticRouter } from 'react-router';
import App from './App';
import { Providers } from './Providers';

export { PRERENDER } from './App';
export { guideSlugs } from './content/guides';

export async function render(url: string): Promise<string> {
  const { prelude } = await prerenderToNodeStream(
    <StaticRouter location={url}>
      <Providers>
        <App />
      </Providers>
    </StaticRouter>,
  );
  const chunks: Buffer[] = [];
  for await (const c of prelude) chunks.push(typeof c === 'string' ? Buffer.from(c) : c);
  return Buffer.concat(chunks).toString('utf8');
}
