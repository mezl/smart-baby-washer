# PlatformIO passes build_flags to the compiler but not the linker on the native
# platform, so --coverage compiles fine and then fails at link with undefined
# __gcov_init. Append it to LINKFLAGS explicitly.
Import("env")
env.Append(LINKFLAGS=["--coverage"])
