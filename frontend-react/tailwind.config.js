/** @type {import('tailwindcss').Config} */
export default {
  content: ['./index.html', './src/**/*.{ts,tsx}'],
  theme: {
    extend: {
      colors: {
        /* Early-2000s IBM workstation: warm industrial greys */
        surface: '#969690',  // body bg — warm grey
        panel:   '#b3b3ac',  // card bg — lighter grey
        grey:    '#7e7e78',  // darker accent grey (status bar, dividers)
        border:  '#000000',  // pure black frames

        /* text — all black for max contrast on grey */
        ink:     '#000000',
        muted:   '#000000',
        subtle:  '#000000',

        /* accents — IBM "big blue" + tuned bull/bear */
        accent:  '#1a3c70',  // deep IBM blue
        bull:    '#1a5f29',  // forest green
        bear:    '#803333',  // brick red
      },
    },
  },
  plugins: [],
};
