@echo off

cl /nologo /GS- /Gs9999999 /Gm- /EHa- /GF /Gy /GA /GR- /Zi /Fe:imgview.exe ..\imgview.cpp ^
	/link /NODEFAULTLIB kernel32.lib user32.lib gdi32.lib ole32.lib shlwapi.lib shell32.lib advapi32.lib comctl32.lib uuid.lib gdiplus.lib ^
	/subsystem:windows /OPT:REF /OPT:ICF /STACK:0x100000,0x100000 ^
  	&& imgview 