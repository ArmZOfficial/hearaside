import { StrictMode } from 'react';
import { createRoot, hydrateRoot } from 'react-dom/client';
import { BrowserRouter } from 'react-router';
import App from './App';
import { Providers } from './Providers';

const root = document.getElementById('root')!;
const tree = (
  <StrictMode>
    <BrowserRouter>
      <Providers>
        <App />
      </Providers>
    </BrowserRouter>
  </StrictMode>
);

// prerendered pages (/, /download, /guides ...) hydrate; the account pages start empty
if (root.firstElementChild) hydrateRoot(root, tree);
else createRoot(root).render(tree);
