@echo off
rem MSVC when available (what CI uses), llvm-mingw otherwise.
rem Add /DMEWLINKS_TRACE (cl) or -DMEWLINKS_TRACE (clang) to log the per-click diagnostic log.
where cl >nul 2>&1
if %errorlevel%==0 (
  rc /nologo /fo version.res version.rc || exit /b 1
  cl /nologo /LD /O2 /EHsc /DNDEBUG /I..\common dllmain.cpp version.res /link /DLL /OUT:mewlinks.dll
) else (
  windres version.rc -O coff -o version.res || exit /b 1
  clang++ -O2 -shared -static -s -Wall -Wextra -I../common -o mewlinks.dll dllmain.cpp version.res
)
