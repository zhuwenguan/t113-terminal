set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(T113_ARM_TARGET ON CACHE INTERNAL "Build for the T113 ARM target")

set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Absolute host-side library paths are useful while linking against the
# staged T113 SDK, but must never become runtime search paths in board ELFs.
set(CMAKE_SKIP_RPATH ON CACHE BOOL
    "Do not embed host build-machine paths in T113 binaries" FORCE)
