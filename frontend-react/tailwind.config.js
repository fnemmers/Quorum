/** @type {import('tailwindcss').Config} */
export default {
  content: ['./index.html', './src/**/*.{ts,tsx}'],
  theme: {
    extend: {
      colors: {
        surface: '#0f1117',
        panel:   '#161b22',
        border:  '#30363d',
        accent:  '#58a6ff',
        bull:    '#3fb950',
        bear:    '#f85149',
      },
    },
  },
  plugins: [],
};
