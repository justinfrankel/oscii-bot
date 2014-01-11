# Microsoft Developer Studio Project File - Name="osciibot" - Package Owner=<4>
# Microsoft Developer Studio Generated Build File, Format Version 6.00
# ** DO NOT EDIT **

# TARGTYPE "Win32 (x86) Application" 0x0101

CFG=osciibot - Win32 Debug
!MESSAGE This is not a valid makefile. To build this project using NMAKE,
!MESSAGE use the Export Makefile command and run
!MESSAGE 
!MESSAGE NMAKE /f "osciibot.mak".
!MESSAGE 
!MESSAGE You can specify a configuration when running NMAKE
!MESSAGE by defining the macro CFG on the command line. For example:
!MESSAGE 
!MESSAGE NMAKE /f "osciibot.mak" CFG="osciibot - Win32 Debug"
!MESSAGE 
!MESSAGE Possible choices for configuration are:
!MESSAGE 
!MESSAGE "osciibot - Win32 Release" (based on "Win32 (x86) Application")
!MESSAGE "osciibot - Win32 Debug" (based on "Win32 (x86) Application")
!MESSAGE 

# Begin Project
# PROP AllowPerConfigDependencies 0
# PROP Scc_ProjName ""
# PROP Scc_LocalPath ""
CPP=cl.exe
MTL=midl.exe
RSC=rc.exe

!IF  "$(CFG)" == "osciibot - Win32 Release"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 0
# PROP BASE Output_Dir "Release"
# PROP BASE Intermediate_Dir "Release"
# PROP BASE Target_Dir ""
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 0
# PROP Output_Dir "Release"
# PROP Intermediate_Dir "Release"
# PROP Ignore_Export_Lib 0
# PROP Target_Dir ""
# ADD BASE CPP /nologo /W3 /GX /O2 /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /D "_MBCS" /YX /FD /c
# ADD CPP /nologo /MD /W3 /GX /O2 /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /D "_MBCS" /YX /FD /opt:nowin98 /c
# ADD BASE MTL /nologo /D "NDEBUG" /mktyplib203 /win32
# ADD MTL /nologo /D "NDEBUG" /mktyplib203 /win32
# ADD BASE RSC /l 0x409 /d "NDEBUG"
# ADD RSC /l 0x409 /d "NDEBUG"
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
LINK32=link.exe
# ADD BASE LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /subsystem:windows /machine:I386
# ADD LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib winmm.lib wsock32.lib /nologo /subsystem:windows /machine:I386 /out:"Release/oscii-bot.exe"

!ELSEIF  "$(CFG)" == "osciibot - Win32 Debug"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 1
# PROP BASE Output_Dir "Debug"
# PROP BASE Intermediate_Dir "Debug"
# PROP BASE Target_Dir ""
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 1
# PROP Output_Dir "Debug"
# PROP Intermediate_Dir "Debug"
# PROP Ignore_Export_Lib 0
# PROP Target_Dir ""
# ADD BASE CPP /nologo /W3 /Gm /GX /ZI /Od /D "WIN32" /D "_DEBUG" /D "_WINDOWS" /D "_MBCS" /YX /FD /GZ /c
# ADD CPP /nologo /W3 /Gm /GX /ZI /Od /D "WIN32" /D "_DEBUG" /D "_WINDOWS" /D "_MBCS" /YX /FD /GZ /c
# ADD BASE MTL /nologo /D "_DEBUG" /mktyplib203 /win32
# ADD MTL /nologo /D "_DEBUG" /mktyplib203 /win32
# ADD BASE RSC /l 0x409 /d "_DEBUG"
# ADD RSC /l 0x409 /d "_DEBUG"
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
LINK32=link.exe
# ADD BASE LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /subsystem:windows /debug /machine:I386 /pdbtype:sept
# ADD LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib winmm.lib wsock32.lib /nologo /subsystem:windows /debug /machine:I386 /out:"Debug/oscii-bot.exe" /pdbtype:sept

!ENDIF 

# Begin Target

# Name "osciibot - Win32 Release"
# Name "osciibot - Win32 Debug"
# Begin Group "Source Files"

# PROP Default_Filter "cpp;c;cxx;rc;def;r;odl;idl;hpj;bat"
# Begin Group "eel2"

# PROP Default_Filter ""
# Begin Source File

SOURCE=..\WDL\eel2\eel_files.h
# End Source File
# Begin Source File

SOURCE=..\WDL\eel2\eel_lice.h
# End Source File
# Begin Source File

SOURCE=..\WDL\eel2\eel_strings.h
# End Source File
# Begin Source File

SOURCE="..\WDL\eel2\nseel-caltab.c"
# End Source File
# Begin Source File

SOURCE="..\WDL\eel2\nseel-cfunc.c"
# End Source File
# Begin Source File

SOURCE="..\WDL\eel2\nseel-compiler.c"
# End Source File
# Begin Source File

SOURCE="..\WDL\eel2\nseel-eval.c"
# End Source File
# Begin Source File

SOURCE="..\WDL\eel2\nseel-lextab.c"
# End Source File
# Begin Source File

SOURCE="..\WDL\eel2\nseel-ram.c"
# End Source File
# Begin Source File

SOURCE="..\WDL\eel2\nseel-yylex.c"
# End Source File
# End Group
# Begin Group "jnetlib"

# PROP Default_Filter ""
# Begin Source File

SOURCE=..\WDL\jnetlib\util.cpp
# End Source File
# End Group
# Begin Group "lice"

# PROP Default_Filter ""
# Begin Group "pnglib"

# PROP Default_Filter ""
# Begin Source File

SOURCE=..\WDL\libpng\png.c
# End Source File
# Begin Source File

SOURCE=..\WDL\libpng\png.h
# End Source File
# Begin Source File

SOURCE=..\WDL\libpng\pngconf.h
# End Source File
# Begin Source File

SOURCE=..\WDL\libpng\pngdebug.h
# End Source File
# Begin Source File

SOURCE=..\WDL\libpng\pngerror.c
# End Source File
# Begin Source File

SOURCE=..\WDL\libpng\pngget.c
# End Source File
# Begin Source File

SOURCE=..\WDL\libpng\pnginfo.h
# End Source File
# Begin Source File

SOURCE=..\WDL\libpng\pnglibconf.h
# End Source File
# Begin Source File

SOURCE=..\WDL\libpng\pngmem.c
# End Source File
# Begin Source File

SOURCE=..\WDL\libpng\pngpread.c
# End Source File
# Begin Source File

SOURCE=..\WDL\libpng\pngpriv.h
# End Source File
# Begin Source File

SOURCE=..\WDL\libpng\pngread.c
# End Source File
# Begin Source File

SOURCE=..\WDL\libpng\pngrio.c
# End Source File
# Begin Source File

SOURCE=..\WDL\libpng\pngrtran.c
# End Source File
# Begin Source File

SOURCE=..\WDL\libpng\pngrutil.c
# End Source File
# Begin Source File

SOURCE=..\WDL\libpng\pngset.c
# End Source File
# Begin Source File

SOURCE=..\WDL\libpng\pngstruct.h
# End Source File
# Begin Source File

SOURCE=..\WDL\libpng\pngtrans.c
# End Source File
# Begin Source File

SOURCE=..\WDL\libpng\pngwio.c
# End Source File
# Begin Source File

SOURCE=..\WDL\libpng\pngwrite.c
# End Source File
# Begin Source File

SOURCE=..\WDL\libpng\pngwtran.c
# End Source File
# Begin Source File

SOURCE=..\WDL\libpng\pngwutil.c
# End Source File
# End Group
# Begin Group "zlib"

# PROP Default_Filter ""
# Begin Source File

SOURCE=..\WDL\zlib\adler32.c
# End Source File
# Begin Source File

SOURCE=..\WDL\zlib\compress.c
# End Source File
# Begin Source File

SOURCE=..\WDL\zlib\crc32.c
# End Source File
# Begin Source File

SOURCE=..\WDL\zlib\crc32.h
# End Source File
# Begin Source File

SOURCE=..\WDL\zlib\deflate.c
# End Source File
# Begin Source File

SOURCE=..\WDL\zlib\deflate.h
# End Source File
# Begin Source File

SOURCE=..\WDL\zlib\gzguts.h
# End Source File
# Begin Source File

SOURCE=..\WDL\zlib\gzlib.c
# End Source File
# Begin Source File

SOURCE=..\WDL\zlib\infback.c
# End Source File
# Begin Source File

SOURCE=..\WDL\zlib\inffast.c
# End Source File
# Begin Source File

SOURCE=..\WDL\zlib\inffast.h
# End Source File
# Begin Source File

SOURCE=..\WDL\zlib\inffixed.h
# End Source File
# Begin Source File

SOURCE=..\WDL\zlib\inflate.c
# End Source File
# Begin Source File

SOURCE=..\WDL\zlib\inflate.h
# End Source File
# Begin Source File

SOURCE=..\WDL\zlib\inftrees.c
# End Source File
# Begin Source File

SOURCE=..\WDL\zlib\inftrees.h
# End Source File
# Begin Source File

SOURCE=..\WDL\zlib\trees.c
# End Source File
# Begin Source File

SOURCE=..\WDL\zlib\trees.h
# End Source File
# Begin Source File

SOURCE=..\WDL\zlib\uncompr.c
# End Source File
# Begin Source File

SOURCE=..\WDL\zlib\zlib.h
# End Source File
# Begin Source File

SOURCE=..\WDL\zlib\zutil.c
# End Source File
# Begin Source File

SOURCE=..\WDL\zlib\zutil.h
# End Source File
# End Group
# Begin Source File

SOURCE=..\WDL\lice\lice.cpp
# End Source File
# Begin Source File

SOURCE=..\WDL\lice\lice_line.cpp
# End Source File
# Begin Source File

SOURCE=..\WDL\lice\lice_png.cpp
# End Source File
# Begin Source File

SOURCE=..\WDL\lice\lice_text.cpp
# End Source File
# Begin Source File

SOURCE=..\WDL\lice\lice_textnew.cpp
# End Source File
# End Group
# Begin Source File

SOURCE=.\device.h
# End Source File
# Begin Source File

SOURCE=.\midi2osc.cpp
# End Source File
# Begin Source File

SOURCE=.\midi_win32.cpp
# End Source File
# Begin Source File

SOURCE=.\oscmsg.cpp
# End Source File
# Begin Source File

SOURCE=.\oscmsg.h
# End Source File
# Begin Source File

SOURCE=.\res.rc
# End Source File
# Begin Source File

SOURCE=..\WDL\wingui\wndsize.cpp
# End Source File
# End Group
# Begin Group "Header Files"

# PROP Default_Filter "h;hpp;hxx;hm;inl"
# Begin Source File

SOURCE=..\WDL\eel2\glue_port.h
# End Source File
# Begin Source File

SOURCE=..\WDL\eel2\glue_ppc.h
# End Source File
# Begin Source File

SOURCE=..\WDL\eel2\glue_x86.h
# End Source File
# Begin Source File

SOURCE=..\WDL\eel2\glue_x86_64.h
# End Source File
# Begin Source File

SOURCE="..\WDL\eel2\ns-eel-addfuncs.h"
# End Source File
# Begin Source File

SOURCE="..\WDL\eel2\ns-eel-int.h"
# End Source File
# Begin Source File

SOURCE="..\WDL\eel2\ns-eel.h"
# End Source File
# End Group
# Begin Group "Resource Files"

# PROP Default_Filter "ico;cur;bmp;dlg;rc2;rct;bin;rgs;gif;jpg;jpeg;jpe"
# Begin Source File

SOURCE=.\icon1.ico
# End Source File
# End Group
# End Target
# End Project
