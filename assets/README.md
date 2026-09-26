# Source art

The originals the generated files under `src/ui/` come from. Kept so those
files can be regenerated rather than only edited, which for a 960KB array of
hex bytes is the difference between a source file and a binary.

- `pegasus-splash.jpeg` — the boot screen, 896x1200.

  ```
  python3 tools/gensplash.py assets/pegasus-splash.jpeg src/ui/SplashImage.c \
      --inset 76 --shift 16 --indexed
  ```

  The tool scales to cover 240x320 and crops the centre. This image is already
  close to 3:4, so the cover crop takes almost nothing.

  The `--inset 76` is the part that matters: the artwork is drawn with a device
  bezel around it, and a bezel inside a real bezel reads as a photograph of the
  thing rather than as the thing. 76 is measured, not guessed -- the drawn
  frame is about 45px of this 896x1200 image and 76 clears it and its inner
  highlight while leaving the title and the footer strip intact. 92 starts
  cutting the title.

  `--shift 16` slides the picture down the frame. The title is drawn hard
  against the top of the artwork, which on a panel means hard against the
  bezel, and that reads as a crop rather than a margin. The cover crop has 9px
  of vertical slack to spend and the flat sky at the top is repeated for the
  other 7. The drawn "SYSTEM INITIALIZING" strip falls off the bottom because
  of it -- it was already being clipped, and half a line of text reads as a
  fault where none reads as a choice.

  `--indexed` halves the array, 150KB to 76KB, with no decoder and no decode
  buffer. Measured before committing to it: 256 colours against this artwork's
  32,581 comes to an RMSE of 3.8 out of 255, which is invisible. Dithering is
  deliberately off -- it would trade banding nobody sees in two seconds for a
  stipple that looks like screen noise.
