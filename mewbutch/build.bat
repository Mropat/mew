@echo off
rem MSVC when available (what CI uses), llvm-mingw otherwise.
rem Add /DMEWBUTCH_TRACE (cl) or -DMEWBUTCH_TRACE (clang) to log each cat the mod accepts on Butch's behalf.
where cl >nul 2>&1
if %errorlevel%==0 (
  rc /nologo /fo version.res version.rc || exit /b 1
  cl /nologo /LD /O2 /EHsc /DNDEBUG /I..\common dllmain.cpp version.res /link /DLL /OUT:mewbutch.dll
) else (
  windres version.rc -O coff -o version.res || exit /b 1
  clang++ -O2 -shared -static -s -Wall -Wextra -I../common -o mewbutch.dll dllmain.cpp version.res
)
