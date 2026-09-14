@echo off
rem Not part of the release flow - build it by hand.
rem Add /DNOFLIES_TRACE (cl) or -DNOFLIES_TRACE (clang) to log every fill.
where cl >nul 2>&1
if %errorlevel%==0 (
  cl /nologo /LD /O2 /EHsc /DNDEBUG /I..\common dllmain.cpp /link /DLL /OUT:no_more_overflow_flies.dll
) else (
  clang++ -O2 -shared -static -s -Wall -Wextra -I../common -o no_more_overflow_flies.dll dllmain.cpp
)
