@echo off
rem MSVC when available (what CI uses), llvm-mingw otherwise.
where cl >nul 2>&1
if %errorlevel%==0 (
  cl /nologo /LD /O2 /EHsc /DNDEBUG dllmain.cpp /link /DLL /OUT:mewdaily.dll
) else (
  clang++ -O2 -shared -static -s -Wall -Wextra -o mewdaily.dll dllmain.cpp
)
