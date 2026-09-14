@echo off
rem MSVC when available (what CI uses), llvm-mingw otherwise. Either works: the
rem mod never includes <string> or <functional>, so it does not depend on the
rem STL it is compiled against. Add /DMEWLINKS_TRACE (cl) or -DMEWLINKS_TRACE
rem (clang) for the per-click diagnostic log.
where cl >nul 2>&1
if %errorlevel%==0 (
  cl /nologo /LD /O2 /EHsc /DNDEBUG /I..\common dllmain.cpp /link /DLL /OUT:mewlinks.dll
) else (
  clang++ -O2 -shared -static -s -Wall -Wextra -I../common -o mewlinks.dll dllmain.cpp
)
