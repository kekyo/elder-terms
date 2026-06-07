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

const parsePositiveInteger = (
  value: string | undefined,
  fallback: number
): number => {
  if (value === undefined) {
    return fallback;
  }

  const parsed = Number.parseInt(value, 10);
  return Number.isInteger(parsed) && parsed > 0 ? parsed : fallback;
};

const parseBoolean = (
  value: string | undefined,
  fallback: boolean
): boolean => {
  if (value === undefined) {
    return fallback;
  }

  const normalized = value.trim().toLowerCase();
  if (['1', 'true', 'yes', 'on'].includes(normalized)) {
    return true;
  }
  if (['0', 'false', 'no', 'off'].includes(normalized)) {
    return false;
  }
  return fallback;
};

const testMaxConcurrency = parsePositiveInteger(
  process.env.ELDER_TERMS_VTE_TEST_MAX_CONCURRENCY,
  16
);
const testFileParallelism = parseBoolean(
  process.env.ELDER_TERMS_VTE_TEST_FILE_PARALLELISM,
  false
);

export default defineConfig({
  plugins: [prettierMax()],
  test: {
    env: {
      ELDER_TERMS_TEST_RESULT_RUN_ID: testResultRunId,
      ELDER_TERMS_VTE_TEST_FILE_PARALLELISM: String(testFileParallelism),
      ELDER_TERMS_VTE_TEST_MAX_CONCURRENCY: String(testMaxConcurrency),
    },
    fileParallelism: testFileParallelism,
    globals: true,
    environment: 'node',
    include: ['tests/**/*.test.ts'],
    maxConcurrency: testMaxConcurrency,
    testTimeout: 30_000,
    coverage: {
      enabled: false,
    },
  },
});
