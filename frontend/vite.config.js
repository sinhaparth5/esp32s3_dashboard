import react from '@vitejs/plugin-react'
import { defineConfig } from 'vite'

export default defineConfig({
  plugins: [react()],
  build: {
    // Fixed names (no hashes) so the firmware can embed them by path. See firmware/main/CMakeLists.txt.
    rolldownOptions: {
      output: { entryFileNames: 'assets/app.js', assetFileNames: 'assets/app[extname]' },
    },
  },
})
