@echo off
cl /nologo /LD /O2 /EHsc /DNDEBUG dllmain.cpp /link /DLL /OUT:mewbutch.dll
