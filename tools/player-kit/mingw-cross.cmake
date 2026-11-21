# Cross-compiling for x86_64 Windows with llvm-mingw from Linux (the player
# kits' build, as Punch-Out's): LLVM_MINGW_CROSS in the environment names the
# unpacked Linux-hosted llvm-mingw release.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
if(NOT DEFINED ENV{LLVM_MINGW_CROSS})
    message(FATAL_ERROR "set LLVM_MINGW_CROSS to the unpacked Linux-hosted llvm-mingw")
endif()
set(llvm_mingw "$ENV{LLVM_MINGW_CROSS}")
set(CMAKE_C_COMPILER "${llvm_mingw}/bin/x86_64-w64-mingw32-clang")
set(CMAKE_CXX_COMPILER "${llvm_mingw}/bin/x86_64-w64-mingw32-clang++")
set(CMAKE_RC_COMPILER "${llvm_mingw}/bin/x86_64-w64-mingw32-windres")
set(CMAKE_AR "${llvm_mingw}/bin/llvm-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${llvm_mingw}/bin/llvm-ranlib" CACHE FILEPATH "")
set(CMAKE_FIND_ROOT_PATH "${llvm_mingw}/x86_64-w64-mingw32")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# Self-contained programs: libc++, libunwind and the compiler runtime linked in.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
