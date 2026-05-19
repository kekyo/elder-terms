import { defineConfig } from 'vitest/config';
import prettierMax from 'prettier-max';

export default defineConfig({
  plugins: [prettierMax()],
  test: {
    globals: true,
    environment: 'node',
    include: ['tests/**/*.test.ts'],
    coverage: {
      enabled: false,
    },
  },
});
