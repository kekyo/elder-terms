import { defineConfig } from 'vitest/config';
import prettierMax from 'prettier-max';

export default defineConfig({
  plugins: [prettierMax()],
  test: {
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
