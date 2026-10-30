// @vitest-environment jsdom
import { act, render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, expect, it, vi } from 'vitest';
import { MemoryRouter } from 'react-router';
import App from '../../src/App';
import { Providers } from '../../src/Providers';
import { startDemo, startSong } from '../../src/motion/demoAudio';

vi.mock('../../src/motion/demoAudio', () => ({ startDemo: vi.fn(), startSong: vi.fn() }));
vi.stubGlobal('IntersectionObserver', class { observe() {} disconnect() {} unobserve() {} });
vi.stubGlobal('matchMedia', (media: string) => ({ matches: false, media, addEventListener() {}, removeEventListener() {} }));
window.scrollTo = () => {};
const player = { stop: vi.fn(), set: vi.fn() };
const renderHome = () => render(<MemoryRouter><Providers><App /></Providers></MemoryRouter>);

beforeEach(() => {
  vi.clearAllMocks();
  vi.mocked(startDemo).mockReturnValue(player);
  vi.mocked(startSong).mockResolvedValue(null);
});

it('stops the demo when navigating away', async () => {
  renderHome();
  await userEvent.click(screen.getByRole('button', { name: /^You$/ }));
  await waitFor(() => expect(startDemo).toHaveBeenCalledOnce());
  await userEvent.click(screen.getAllByRole('link', { name: 'Guides' })[0]);
  await waitFor(() => expect(player.stop).toHaveBeenCalledOnce());
});

it('disposes a player that finishes loading after navigation', async () => {
  let resolveSong!: (value: typeof player) => void;
  vi.mocked(startSong).mockImplementation(() => new Promise(resolve => { resolveSong = resolve; }));
  renderHome();
  await userEvent.click(screen.getByRole('button', { name: /^You$/ }));
  await waitFor(() => expect(startSong).toHaveBeenCalledOnce());
  await userEvent.click(screen.getAllByRole('link', { name: 'Guides' })[0]);
  await act(async () => { resolveSong(player); });
  expect(player.stop).toHaveBeenCalledOnce();
  expect(startDemo).not.toHaveBeenCalled();
});
