import tsParser from '@typescript-eslint/parser'
import reactHooks from 'eslint-plugin-react-hooks'

export default [
  {
    ignores: [
      'dist/**',
      'coverage/**',
      'node_modules/**',
      'src/**/*.test.{ts,tsx}',
      'src/test-setup.ts',
    ],
  },
  {
    files: ['src/**/*.{ts,tsx}'],
    languageOptions: {
      parser: tsParser,
      parserOptions: {
        ecmaVersion: 'latest',
        sourceType: 'module',
        ecmaFeatures: { jsx: true },
      },
    },
    plugins: { 'react-hooks': reactHooks },
    rules: {
      // Full Rules-of-React suite (rules-of-hooks, purity, immutability,
      // preserve-manual-memoization, etc.) as errors — these are what the React
      // Compiler relies on, and the codebase is already clean against them.
      ...reactHooks.configs['recommended-latest'].rules,
      // Downgraded to warnings: pre-existing patterns spread across the app that
      // are not compiler bailouts. Kept visible so they can be burned down, but
      // they don't block the pre-push gate.
      //   set-state-in-effect: state synced from props/URL inside effects
      //   refs: FLIP-style ref read during render (QueuePopup)
      //   exhaustive-deps: several deliberately narrowed dep arrays
      'react-hooks/set-state-in-effect': 'warn',
      'react-hooks/refs': 'warn',
      'react-hooks/exhaustive-deps': 'warn',
    },
  },
]
