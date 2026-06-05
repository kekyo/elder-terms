import { defineConfig } from 'vitest/config';
import prettierMax from 'prettier-max';

const formatTestResultRunId = (date: Date): string => {
  const pad = (value: number, length: number): string =>
    value.toString().padStart(length, '0');

  return [
    `${pad(date.getFullYear(), 4)}${pad(date.getMonth() + 1, 2)}${pad(
      date.getDate(),
      2
    )}`,
    `${pad(date.getHours(), 2)}${pad(date.getMinutes(), 2)}${pad(
      date.getSeconds(),
      2
    )}`,
    pad(date.getMilliseconds(), 3),
  ].join('_');
};

const testResultRunId =
  process.env.ELDER_TERMS_TEST_RESULT_RUN_ID ??
  formatTestResultRunId(new Date());
process.env.ELDER_TERMS_TEST_RESULT_RUN_ID = testResultRunId;

export default defineConfig({
  plugins: [prettierMax()],
  test: {
    env: {
      ELDER_TERMS_TEST_RESULT_RUN_ID: testResultRunId,
    },
    globals: true,
    environment: 'node',
    include: ['tests/**/*.test.ts'],
    testTimeout: 30_000,
    coverage: {
      enabled: false,
    },
  },
});
