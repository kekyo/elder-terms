import { defineConfig } from 'vitest/config';
import prettierMax from 'prettier-max';
import { getTestFontEnvironment } from '../scripts/test-font-environment.mjs';

export default defineConfig({
  plugins: [prettierMax()],
  test: {
    env: getTestFontEnvironment(),
    fileParallelism: false,
    globals: true,
    environment: 'node',
    include: ['tests/**/*.test.ts'],
    testTimeout: 30_000,
    coverage: {
      enabled: false,
    },
  },
});
