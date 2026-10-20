import { describe, expect, it } from 'vitest';
import { handler } from '../../netlify/functions/api';

describe('Netlify serverless function handler', () => {
  it('responds to /api/v1/health with 200 and { ok: true }', async () => {
    const res = await handler(
      {
        httpMethod: 'GET',
        path: '/api/v1/health',
        headers: {},
        queryStringParameters: null,
        body: null,
      },
      {} as any
    ) as any;

    expect(res.statusCode).toBe(200);
    const body = JSON.parse(res.body);
    expect(body).toEqual({ ok: true });
  });

  it('normalizes /.netlify/functions/api/v1/health prefix to /api/v1/health', async () => {
    const res = await handler(
      {
        httpMethod: 'GET',
        path: '/.netlify/functions/api/v1/health',
        headers: {},
        queryStringParameters: null,
        body: null,
      },
      {} as any
    ) as any;

    expect(res.statusCode).toBe(200);
    const body = JSON.parse(res.body);
    expect(body).toEqual({ ok: true });
  });

  it('returns 404 for unknown api routes', async () => {
    const res = await handler(
      {
        httpMethod: 'GET',
        path: '/api/unknown-endpoint',
        headers: {},
        queryStringParameters: null,
        body: null,
      },
      {} as any
    ) as any;

    expect(res.statusCode).toBe(404);
  });
});
