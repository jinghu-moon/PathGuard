import { defineConfig } from 'vite'

export default defineConfig({
    build: {
        outDir: '../module/webroot',
        emptyOutDir: true,
        sourcemap: false,
        rollupOptions: {
            output: {
                manualChunks(id) {
                    if (id.includes('node_modules')) {
                        if (id.includes('mdui')) {
                            return 'mdui';
                        }
                        return 'vendor';
                    }
                }
            }
        }
    },
    server: {
        port: 1234,
        open: true
    }
})
