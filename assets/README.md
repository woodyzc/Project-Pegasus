# Source art

The originals the generated files under `src/ui/` come from. Kept so those
files can be regenerated rather than only edited, which for a 960KB array of
hex bytes is the difference between a source file and a binary.

- `pegasus-splash.jpeg` — the boot screen, 896x1200.

  ```
  python3 tools/gensplash.py assets/pegasus-splash.jpeg src/ui/SplashImage.c
  ```

  The tool scales to cover 240x320 and crops the centre. This image is already
  close to 3:4, so almost nothing is lost.
