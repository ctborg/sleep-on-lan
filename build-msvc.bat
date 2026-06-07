@echo off
setlocal

cl /nologo /W4 /O2 /D_CRT_SECURE_NO_WARNINGS src\sol.c /Fe:sol.exe ws2_32.lib iphlpapi.lib
