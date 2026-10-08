"""Injects a fresh build stamp as a compiler flag, every build.

Why this exists, in full, because the two obvious alternatives were both
tried on hardware and both lied:

  * __DATE__ / __TIME__ are baked into whichever translation unit contains
    them. Change one other file and PlatformIO rebuilds only that file, so
    the stamp stays at whenever the stamp's own file last compiled. Two
    photographs of the panel showed an identical build time across firmwares
    whose measured behaviour differed threefold.

  * esp_ota_get_app_description() reads the image header, which sounds
    authoritative and is -- for somebody else's build. Under PlatformIO the
    Arduino core arrives precompiled, so that descriptor reads
    project 'arduino-lib-builder', 'Mar  5 2024', and an ELF SHA that never
    changes no matter what is compiled. Strictly worse: always wrong rather
    than sometimes stale.

A flag computed here is regenerated on every invocation, and because its
value changes, the file that uses it is recompiled. That is the property the
other two lacked.

The line this feeds answers "did the flash actually take", which is the
question every hardware debugging session in this project starts with.
"""

import datetime

Import("env")

stamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
env.Append(CPPDEFINES=[("PEGASUS_BUILD_STAMP", env.StringifyMacro(stamp))])
