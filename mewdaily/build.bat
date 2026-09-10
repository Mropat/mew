@echo off
REM Builds mewdaily.dll. Run from an x64 Native Tools Command Prompt,
REM or let .github/workflows/build.yml do it.
cl /nologo /LD /O2 /EHsc /DNDEBUG dllmain.cpp /link /DLL /OUT:mewdaily.dll
