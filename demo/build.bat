@echo off
REM Build script for xnet demos (Windows - MSVC cl compiler)
REM Usage: build.bat [clean|debug|all|<demo_name>]
REM   clean      - Clean all build artifacts
REM   debug      - Compile Debug version (default Release)
REM   all        - Build all demos
REM   <demo_name>- Build specific demo

setlocal enabledelayedexpansion

REM ============================================
REM VS Environment Setup
REM ============================================

for %%P in (
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\software\MicrosoftVisual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "D:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "D:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
    "D:\Program Files\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "D:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "D:\Program Files\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "D:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvarsall.bat"
) do (
    if exist "%%P" (
        echo Found: %%P
        set "VS_VCVARS=%%P"
        goto :found_vs
    )
)

:found_vs
if not defined VS_VCVARS (
    echo Visual Studio not found in common locations
    echo Please install Visual Studio or run from Developer Command Prompt
    pause
    exit /b 1
)

call !VS_VCVARS! x64

where cl >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo Error: cl.exe not found in PATH
    echo Please run this script from "Developer Command Prompt for VS"
    exit /b 1
)

REM ============================================
REM Build Configuration
REM ============================================
set "BUILD_MODE=release"
set "CFLAGS_COMMON=/W3 /utf-8 /I.. /I"..\3rd" /D_WIN32 /D_WIN64 /D_CRT_SECURE_NO_WARNINGS /nologo"
set "CXXFLAGS_COMMON=/W3 /utf-8 /std:c++20 /EHsc /I.. /I"..\3rd" /D_WIN32 /D_WIN64 /D_CRT_SECURE_NO_WARNINGS /nologo"
set "LDFLAGS=ws2_32.lib dbghelp.lib"

REM Check first argument
set "TARGET=%~1"
if "%TARGET%"=="debug" (
    set "BUILD_MODE=debug"
    set "CFLAGS=!CFLAGS_COMMON! /Od /Zi /DDEBUG /MDd"
    set "CXXFLAGS=!CXXFLAGS_COMMON! /Od /Zi /DDEBUG /MDd"
    echo Building [DEBUG] x64 with cl compiler...
    set "TARGET=all"
) else if "%TARGET%"=="clean" (
    goto :do_clean
) else if "%TARGET%"=="" (
    set "TARGET=all"
    set "CFLAGS=!CFLAGS_COMMON! /O2 /MT"
    set "CXXFLAGS=!CXXFLAGS_COMMON! /O2 /MT"
    echo Building [RELEASE] x64 with cl compiler...
) else (
    set "CFLAGS=!CFLAGS_COMMON! /O2 /MT"
    set "CXXFLAGS=!CXXFLAGS_COMMON! /O2 /MT"
    echo Building [RELEASE] x64: %TARGET% ...
)

REM ============================================
REM Directory Setup
REM ============================================
set "BIN_DIR=..\bin"
set "OBJ_DIR=.obj"
set "XNET_OBJ_DIR=!OBJ_DIR!\xnet"

if not exist "!BIN_DIR!" mkdir "!BIN_DIR!"
if not exist "!OBJ_DIR!" mkdir "!OBJ_DIR!"
if not exist "!XNET_OBJ_DIR!" mkdir "!XNET_OBJ_DIR!"

REM ============================================
REM Demo List
REM ============================================
set DEMOS=xhttpd_svr xrpc_server xrpc_client xthread_demo xnet_client xnet_svr xnet_client_coroutine xnet_svr_coroutine xnet_svr_iocp xnet_coroutine xpac_server xredis_client xcoroutine_exception xthread_aeweakup svr

REM ============================================
REM Compile xnet library files
REM ============================================
echo.
echo Compiling xnet library files...
echo.

if not exist "!XNET_OBJ_DIR!\ae.obj" (
    echo Compiling ae.c...
    cl !CFLAGS! /c ..\ae.c /Fo"!XNET_OBJ_DIR!\ae.obj" || goto :error
)

if not exist "!XNET_OBJ_DIR!\anet.obj" (
    echo Compiling anet.c...
    cl !CFLAGS! /c ..\anet.c /Fo"!XNET_OBJ_DIR!\anet.obj" || goto :error
)

if not exist "!XNET_OBJ_DIR!\zmalloc.obj" (
    echo Compiling zmalloc.c...
    cl !CFLAGS! /c ..\zmalloc.c /Fo"!XNET_OBJ_DIR!\zmalloc.obj" || goto :error
)

if not exist "!XNET_OBJ_DIR!\xlog.obj" (
    echo Compiling xlog.c...
    cl !CFLAGS! /c ..\xlog.c /Fo"!XNET_OBJ_DIR!\xlog.obj" || goto :error
)

if not exist "!XNET_OBJ_DIR!\xtimer.obj" (
    echo Compiling xtimer.c...
    cl !CFLAGS! /c ..\xtimer.c /Fo"!XNET_OBJ_DIR!\xtimer.obj" || goto :error
)

if not exist "!XNET_OBJ_DIR!\xchannel.obj" (
    echo Compiling xchannel.cpp...
    cl !CXXFLAGS! /c ..\xchannel.cpp /Fo"!XNET_OBJ_DIR!\xchannel.obj" || goto :error
)

if not exist "!XNET_OBJ_DIR!\xchannel_pdu.obj" (
    echo Compiling xchannel_pdu.cpp...
    cl !CXXFLAGS! /c ..\xchannel_pdu.cpp /Fo"!XNET_OBJ_DIR!\xchannel_pdu.obj" || goto :error
)

if not exist "!XNET_OBJ_DIR!\xcoroutine.obj" (
    echo Compiling xcoroutine.cpp...
    cl !CXXFLAGS! /c ..\xcoroutine.cpp /Fo"!XNET_OBJ_DIR!\xcoroutine.obj" || goto :error
)

if not exist "!XNET_OBJ_DIR!\xhandle.obj" (
    echo Compiling xhandle.cpp...
    cl !CXXFLAGS! /c ..\xhandle.cpp /Fo"!XNET_OBJ_DIR!\xhandle.obj" || goto :error
)

if not exist "!XNET_OBJ_DIR!\xhttpd.obj" (
    echo Compiling xhttpd.cpp...
    cl !CXXFLAGS! /c ..\xhttpd.cpp /Fo"!XNET_OBJ_DIR!\xhttpd.obj" || goto :error
)

if not exist "!XNET_OBJ_DIR!\xrpc.obj" (
    echo Compiling xrpc.cpp...
    cl !CXXFLAGS! /c ..\xrpc.cpp /Fo"!XNET_OBJ_DIR!\xrpc.obj" || goto :error
)

if not exist "!XNET_OBJ_DIR!\xthread.obj" (
    echo Compiling xthread.cpp...
    cl !CXXFLAGS! /c ..\xthread.cpp /Fo"!XNET_OBJ_DIR!\xthread.obj" || goto :error
)

if not exist "!XNET_OBJ_DIR!\xredis.obj" (
    echo Compiling xredis.cpp...
    cl !CXXFLAGS! /c ..\xredis.cpp /Fo"!XNET_OBJ_DIR!\xredis.obj" || goto :error
)

if not exist "!XNET_OBJ_DIR!\picohttpparser.obj" (
    echo Compiling picohttpparser.c...
    cl !CFLAGS! /c ..\3rd\picohttpparser.c /Fo"!XNET_OBJ_DIR!\picohttpparser.obj" || goto :error
)

REM ============================================
REM Build requested target
REM ============================================
if "%TARGET%"=="all" (
    echo.
    echo Building all demos...
    echo.
    for %%D in (!DEMOS!) do (
        call :build_demo %%D
        if !ERRORLEVEL! neq 0 goto :error
    )
    echo.
    echo ========================================
    echo All demos built successfully!
    echo Output: !BIN_DIR!
    echo ========================================
    goto :cleanup
) else (
    call :build_demo %TARGET%
    if !ERRORLEVEL! neq 0 goto :error
    goto :cleanup
)

REM ============================================
REM Build a single demo
REM ============================================
:build_demo
set "DEMO=%~1"
echo Building !DEMO!...

if "!DEMO!"=="xhttpd_svr" (
    set "SRC=xhttpd_svr.cpp"
    set "OBJS=!XNET_OBJ_DIR!\picohttpparser.obj !XNET_OBJ_DIR!\ae.obj !XNET_OBJ_DIR!\anet.obj !XNET_OBJ_DIR!\zmalloc.obj !XNET_OBJ_DIR!\xlog.obj !XNET_OBJ_DIR!\xtimer.obj !XNET_OBJ_DIR!\xchannel.obj !XNET_OBJ_DIR!\xchannel_pdu.obj !XNET_OBJ_DIR!\xcoroutine.obj !XNET_OBJ_DIR!\xhandle.obj !XNET_OBJ_DIR!\xhttpd.obj !XNET_OBJ_DIR!\xrpc.obj !XNET_OBJ_DIR!\xthread.obj"
) else if "!DEMO!"=="xrpc_server" (
    set "SRC=xrpc_server.cpp"
    set "OBJS=!XNET_OBJ_DIR!\ae.obj !XNET_OBJ_DIR!\anet.obj !XNET_OBJ_DIR!\zmalloc.obj !XNET_OBJ_DIR!\xlog.obj !XNET_OBJ_DIR!\xtimer.obj !XNET_OBJ_DIR!\xchannel.obj !XNET_OBJ_DIR!\xchannel_pdu.obj !XNET_OBJ_DIR!\xcoroutine.obj !XNET_OBJ_DIR!\xhandle.obj !XNET_OBJ_DIR!\xrpc.obj !XNET_OBJ_DIR!\xthread.obj"
) else if "!DEMO!"=="xrpc_client" (
    set "SRC=xrpc_client.cpp"
    set "OBJS=!XNET_OBJ_DIR!\ae.obj !XNET_OBJ_DIR!\anet.obj !XNET_OBJ_DIR!\zmalloc.obj !XNET_OBJ_DIR!\xlog.obj !XNET_OBJ_DIR!\xtimer.obj !XNET_OBJ_DIR!\xchannel.obj !XNET_OBJ_DIR!\xchannel_pdu.obj !XNET_OBJ_DIR!\xcoroutine.obj !XNET_OBJ_DIR!\xhandle.obj !XNET_OBJ_DIR!\xrpc.obj !XNET_OBJ_DIR!\xthread.obj"
) else if "!DEMO!"=="xthread_demo" (
    set "SRC=xthread_demo.cpp"
    set "OBJS=!XNET_OBJ_DIR!\ae.obj !XNET_OBJ_DIR!\anet.obj !XNET_OBJ_DIR!\zmalloc.obj !XNET_OBJ_DIR!\xlog.obj !XNET_OBJ_DIR!\xtimer.obj !XNET_OBJ_DIR!\xchannel.obj !XNET_OBJ_DIR!\xchannel_pdu.obj !XNET_OBJ_DIR!\xcoroutine.obj !XNET_OBJ_DIR!\xhandle.obj !XNET_OBJ_DIR!\xrpc.obj !XNET_OBJ_DIR!\xthread.obj"
) else if "!DEMO!"=="xnet_client" (
    set "SRC=xnet_client.cpp"
    set "OBJS=!XNET_OBJ_DIR!\ae.obj !XNET_OBJ_DIR!\anet.obj !XNET_OBJ_DIR!\zmalloc.obj !XNET_OBJ_DIR!\xlog.obj !XNET_OBJ_DIR!\xtimer.obj"
) else if "!DEMO!"=="xnet_svr" (
    set "SRC=xnet_svr.c"
    set "OBJS=!XNET_OBJ_DIR!\ae.obj !XNET_OBJ_DIR!\anet.obj !XNET_OBJ_DIR!\zmalloc.obj !XNET_OBJ_DIR!\xlog.obj !XNET_OBJ_DIR!\xtimer.obj"
) else if "!DEMO!"=="xnet_client_coroutine" (
    set "SRC=xnet_client_coroutine.cpp"
    set "OBJS=!XNET_OBJ_DIR!\ae.obj !XNET_OBJ_DIR!\anet.obj !XNET_OBJ_DIR!\zmalloc.obj !XNET_OBJ_DIR!\xlog.obj !XNET_OBJ_DIR!\xtimer.obj !XNET_OBJ_DIR!\xchannel.obj !XNET_OBJ_DIR!\xchannel_pdu.obj !XNET_OBJ_DIR!\xcoroutine.obj !XNET_OBJ_DIR!\xhandle.obj !XNET_OBJ_DIR!\xrpc.obj !XNET_OBJ_DIR!\xthread.obj"
) else if "!DEMO!"=="xnet_svr_coroutine" (
    set "SRC=xnet_svr_coroutine.cpp"
    set "OBJS=!XNET_OBJ_DIR!\ae.obj !XNET_OBJ_DIR!\anet.obj !XNET_OBJ_DIR!\zmalloc.obj !XNET_OBJ_DIR!\xlog.obj !XNET_OBJ_DIR!\xtimer.obj !XNET_OBJ_DIR!\xchannel.obj !XNET_OBJ_DIR!\xchannel_pdu.obj !XNET_OBJ_DIR!\xcoroutine.obj !XNET_OBJ_DIR!\xhandle.obj !XNET_OBJ_DIR!\xrpc.obj !XNET_OBJ_DIR!\xthread.obj"
) else if "!DEMO!"=="xnet_svr_iocp" (
    set "SRC=xnet_svr_iocp.c"
    set "OBJS=!XNET_OBJ_DIR!\ae.obj !XNET_OBJ_DIR!\anet.obj !XNET_OBJ_DIR!\zmalloc.obj !XNET_OBJ_DIR!\xlog.obj !XNET_OBJ_DIR!\xtimer.obj !XNET_OBJ_DIR!\xchannel.obj !XNET_OBJ_DIR!\xchannel_pdu.obj !XNET_OBJ_DIR!\xcoroutine.obj !XNET_OBJ_DIR!\xhandle.obj !XNET_OBJ_DIR!\xrpc.obj !XNET_OBJ_DIR!\xthread.obj"
) else if "!DEMO!"=="xnet_coroutine" (
    set "SRC=xnet_coroutine.cpp"
    set "OBJS=!XNET_OBJ_DIR!\ae.obj !XNET_OBJ_DIR!\anet.obj !XNET_OBJ_DIR!\zmalloc.obj !XNET_OBJ_DIR!\xlog.obj !XNET_OBJ_DIR!\xtimer.obj !XNET_OBJ_DIR!\xchannel.obj !XNET_OBJ_DIR!\xchannel_pdu.obj !XNET_OBJ_DIR!\xcoroutine.obj !XNET_OBJ_DIR!\xhandle.obj !XNET_OBJ_DIR!\xrpc.obj !XNET_OBJ_DIR!\xthread.obj"
) else if "!DEMO!"=="xpac_server" (
    set "SRC=xpac_server.cpp"
    set "OBJS=!XNET_OBJ_DIR!\picohttpparser.obj !XNET_OBJ_DIR!\ae.obj !XNET_OBJ_DIR!\anet.obj !XNET_OBJ_DIR!\zmalloc.obj !XNET_OBJ_DIR!\xlog.obj !XNET_OBJ_DIR!\xtimer.obj !XNET_OBJ_DIR!\xchannel.obj !XNET_OBJ_DIR!\xchannel_pdu.obj !XNET_OBJ_DIR!\xcoroutine.obj !XNET_OBJ_DIR!\xhandle.obj !XNET_OBJ_DIR!\xhttpd.obj !XNET_OBJ_DIR!\xrpc.obj !XNET_OBJ_DIR!\xthread.obj"
) else if "!DEMO!"=="xredis_client" (
    set "SRC=xredis_client.cpp"
    set "OBJS=!XNET_OBJ_DIR!\ae.obj !XNET_OBJ_DIR!\anet.obj !XNET_OBJ_DIR!\zmalloc.obj !XNET_OBJ_DIR!\xlog.obj !XNET_OBJ_DIR!\xtimer.obj !XNET_OBJ_DIR!\xchannel.obj !XNET_OBJ_DIR!\xchannel_pdu.obj !XNET_OBJ_DIR!\xcoroutine.obj !XNET_OBJ_DIR!\xhandle.obj !XNET_OBJ_DIR!\xredis.obj !XNET_OBJ_DIR!\xrpc.obj !XNET_OBJ_DIR!\xthread.obj"
) else if "!DEMO!"=="xcoroutine_exception" (
    set "SRC=xcoroutine_exception.cpp"
    set "OBJS=!XNET_OBJ_DIR!\ae.obj !XNET_OBJ_DIR!\anet.obj !XNET_OBJ_DIR!\zmalloc.obj !XNET_OBJ_DIR!\xlog.obj !XNET_OBJ_DIR!\xtimer.obj !XNET_OBJ_DIR!\xcoroutine.obj !XNET_OBJ_DIR!\xthread.obj"
) else if "!DEMO!"=="xthread_aeweakup" (
    set "SRC=xthread_aeweakup.cpp"
    set "OBJS=!XNET_OBJ_DIR!\ae.obj !XNET_OBJ_DIR!\anet.obj !XNET_OBJ_DIR!\zmalloc.obj !XNET_OBJ_DIR!\xlog.obj !XNET_OBJ_DIR!\xtimer.obj !XNET_OBJ_DIR!\xchannel.obj !XNET_OBJ_DIR!\xchannel_pdu.obj !XNET_OBJ_DIR!\xcoroutine.obj !XNET_OBJ_DIR!\xhandle.obj !XNET_OBJ_DIR!\xrpc.obj !XNET_OBJ_DIR!\xthread.obj"
) else if "!DEMO!"=="svr" (
    set "SRC=svr.c"
    set "OBJS=!XNET_OBJ_DIR!\ae.obj !XNET_OBJ_DIR!\anet.obj !XNET_OBJ_DIR!\zmalloc.obj !XNET_OBJ_DIR!\xlog.obj !XNET_OBJ_DIR!\xtimer.obj"
) else (
    echo Error: Unknown demo '!DEMO!'
    exit /b 1
)

set "DEMO_OBJ=!OBJ_DIR!\!DEMO!.obj"

REM Compile demo source
if "!SRC:~-4!"==".cpp" (
    cl !CXXFLAGS! /c !SRC! /Fo"!DEMO_OBJ!" || exit /b 1
) else (
    if "!DEMO!"=="xnet_svr_iocp" (
        cl !CXXFLAGS! /TP /c !SRC! /Fo"!DEMO_OBJ!" || exit /b 1
    ) else (
        cl !CFLAGS! /c !SRC! /Fo"!DEMO_OBJ!" || exit /b 1
    )
)

REM Link
echo Linking !DEMO!.exe...
link /OUT:"!BIN_DIR!\!DEMO!.exe" !DEMO_OBJ! !OBJS! !LDFLAGS! || exit /b 1

echo [OK] !DEMO! built!
exit /b 0

REM ============================================
REM Clean
REM ============================================
:do_clean
echo Cleaning build artifacts...
if exist "!OBJ_DIR!" rmdir /s /q "!OBJ_DIR!"
for %%D in (!DEMOS!) do (
    if exist "!BIN_DIR!\%%D.exe" del "!BIN_DIR!\%%D.exe"
    if exist "!BIN_DIR!\%%D.pdb" del "!BIN_DIR!\%%D.pdb"
)
echo Cleaned!
goto :eof

REM ============================================
REM Cleanup intermediate files
REM ============================================
:cleanup
echo.
echo Cleaning intermediate files...
if exist "!OBJ_DIR!" rmdir /s /q "!OBJ_DIR!"
goto :eof

REM ============================================
REM Error handler
REM ============================================
:error
echo.
echo ========================================
echo Build failed!
echo ========================================
exit /b 1

:eof
endlocal
