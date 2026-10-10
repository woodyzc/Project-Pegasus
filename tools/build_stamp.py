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

ONLY that file. The first version appended the flag to the whole build
environment, and a define whose value changes every run changes every
compile command -- so SCons rebuilt all 528 objects on every build, LVGL,
NimBLE and the Arduino core included: 37-44 seconds for a build with nothing
edited (measured 2026-10-10). A build middleware attaches it to the one
object that reads it, so an unchanged tree recompiles one file and relinks.

The line this feeds answers "did the flash actually take", which is the
question every hardware debugging session in this project starts with.
"""

import datetime

Import("env")

STAMPED_SOURCE = "Page_Settings.cpp"

stamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def add_stamp(env, node):
    if node.name != STAMPED_SOURCE:
        return node
    # list(): this SCons keeps CPPDEFINES as a deque, which will not
    # concatenate with a list.
    return env.Object(
        node,
        CPPDEFINES=list(env["CPPDEFINES"])
        + [("PEGASUS_BUILD_STAMP", env.StringifyMacro(stamp))],
    )


env.AddBuildMiddleware(add_stamp)
