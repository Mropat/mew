@echo off
rem MSVC when available (what CI uses), llvm-mingw otherwise.
rem Add /DMEWBUTCH_TRACE (cl) or -DMEWBUTCH_TRACE (clang) to log each cat that
rem the mod accepts on Butch's behalf.
where cl >nul 2>&1
if %errorlevel%==0 (
  cl /nologo /LD /O2 /EHsc /DNDEBUG /I..\common dllmain.cpp /link /DLL /OUT:mewbutch.dll
) else (
  clang++ -O2 -shared -static -s -Wall -Wextra -I../common -o mewbutch.dll dllmain.cpp
)
