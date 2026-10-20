import express from 'express';
import serverless from 'serverless-http';
import type { Config } from '@netlify/functions';
import { createApp } from '../../server/app.js';

let serverlessHandler: ReturnType<typeof serverless> | null = null;

export function getHandler() {
  if (!serverlessHandler) {
    const innerApp = createApp();
    const netlifyApp = express();

    // Normalize path if Netlify forwards /.netlify/functions/api/...
    netlifyApp.use((req, _res, next) => {
      if (req.url.startsWith('/.netlify/functions/api')) {
        req.url = req.url.replace(/^\/\.netlify\/functions\/api/, '/api');
      }
      next();
    });
    netlifyApp.use(innerApp);

    serverlessHandler = serverless(netlifyApp);
  }
  return serverlessHandler;
}

export const handler = async (event: any, context: any) => {
  const h = getHandler();
  return h(event, context);
};

export const config: Config = {
  path: '/api/*',
  preferStatic: true,
};
