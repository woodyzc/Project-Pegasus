# Source art

The originals the generated files under `src/ui/` come from. Kept so those
files can be regenerated rather than only edited, which for a 960KB array of
hex bytes is the difference between a source file and a binary.

- `pegasus-splash.jpeg` — the boot screen, 896x1200.

  ```
  python3 tools/gensplash.py assets/pegasus-splash.jpeg src/ui/SplashImage.c --inset 76
  ```

  The tool scales to cover 240x320 and crops the centre. This image is already
  close to 3:4, so the cover crop takes almost nothing.

  The `--inset 76` is the part that matters: the artwork is drawn with a device
  bezel around it, and a bezel inside a real bezel reads as a photograph of the
  thing rather than as the thing. 76 is measured, not guessed -- the drawn
  frame is about 45px of this 896x1200 image and 76 clears it and its inner
  highlight while leaving the title and the footer strip intact. 92 starts
  cutting the title.
