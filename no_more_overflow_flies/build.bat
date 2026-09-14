@echo off
rem MSVC when available (what CI uses), llvm-mingw otherwise.
rem Add /DNOFLIES_TRACE (cl) or -DNOFLIES_TRACE (clang) to log every fill.
where cl >nul 2>&1
if %errorlevel%==0 (
  rc /nologo /fo version.res version.rc || exit /b 1
  cl /nologo /LD /O2 /EHsc /DNDEBUG /I..\common dllmain.cpp version.res /link /DLL /OUT:no_more_overflow_flies.dll
) else (
  windres version.rc -O coff -o version.res || exit /b 1
  clang++ -O2 -shared -static -s -Wall -Wextra -I../common -o no_more_overflow_flies.dll dllmain.cpp version.res
)
