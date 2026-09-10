@echo off
REM Built by CI and locally. Run from an x64 Native Tools Command Prompt.
cl /nologo /LD /O2 /EHsc /DNDEBUG dllmain.cpp /link /DLL /DEF:winmm.def /OUT:winmm.dll
