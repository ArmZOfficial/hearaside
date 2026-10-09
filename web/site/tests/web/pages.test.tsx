// @vitest-environment jsdom
import { describe, expect, it, vi } from 'vitest';
import { render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { MemoryRouter } from 'react-router';
import App from '../../src/App';
import { Providers } from '../../src/Providers';

vi.stubGlobal('IntersectionObserver', class { observe() {} disconnect() {} unobserve() {} });
vi.stubGlobal('matchMedia', (q: string) => ({ matches: false, media: q, addEventListener() {}, removeEventListener() {}, addListener() {}, removeListener() {} }));
window.scrollTo = () => {};

const at = (url: string) => render(<MemoryRouter initialEntries={[url]}><Providers><App /></Providers></MemoryRouter>);

describe('pages', () => {
  it('home: the mock’s headline and the demo switches change the summary', async () => {
    at('/');
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Hear one mix.Stream another.');
    const viewersVocal = screen.getByRole('button', { name: 'Viewers hear Vocal' });
    expect(viewersVocal).toHaveAttribute('aria-pressed', 'true');
    await userEvent.click(viewersVocal);
    expect(viewersVocal).toHaveAttribute('aria-pressed', 'false');
    expect(viewersVocal).toHaveAttribute('title', 'Viewers don’t hear this (click to turn on)');
  });

  it('home in Thai under /th', async () => {
    at('/th');
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('ฟังมิกซ์หนึ่ง');
  });

  it('sign-up: "Create account" stays off until everything is filled in and agreed', async () => {
    at('/signup');
    const button = await screen.findByRole('button', { name: 'Create account' });
    expect(button).toBeDisabled();
    await userEvent.type(screen.getByLabelText('Display name'), 'Mint');
    await userEvent.type(screen.getByLabelText('Email'), 'mint@example.test');
    await userEvent.type(screen.getByLabelText('Password'), 'short');
    expect(screen.getByText('5 more characters')).toBeInTheDocument();
    await userEvent.type(screen.getByLabelText('Password'), ' but now long');
    await userEvent.click(screen.getByRole('checkbox'));
    expect(button).toBeEnabled();
  });

  it('guides list both languages from MDX', async () => {
    at('/th/guides');
    expect(await screen.findByText('เริ่มต้นใช้งาน')).toBeInTheDocument();
    expect(screen.getByText('แก้ปัญหาที่พบบ่อย')).toBeInTheDocument();
  });

  it('account pages send signed-out visitors to sign in', async () => {
    at('/account/devices');
    expect(await screen.findByRole('heading', { name: 'Welcome back' })).toBeInTheDocument();
  });

  it('unknown pages say so', async () => {
    at('/no-such-page');
    expect(await screen.findByText('This page isn’t here')).toBeInTheDocument();
  });
});
