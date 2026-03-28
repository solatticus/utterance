@echo off
setlocal

:: Find VS Build Tools vcvarsall.bat
set "VCVARS="
for %%d in (
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
) do (
    if exist %%d set "VCVARS=%%~d"
)

if "%VCVARS%"=="" (
    echo ERROR: Cannot find vcvarsall.bat. Install Visual Studio Build Tools.
    exit /b 1
)

:: Only call vcvarsall if cl.exe isn't already on PATH
where cl >nul 2>&1
if errorlevel 1 (
    echo Setting up MSVC environment...
    call "%VCVARS%" x64 >nul 2>&1
)

set SRC=src\main.c src\window.c src\camera.c src\font.c src\text.c src\render.c src\fx.c src\segment.c src\linebreak.c src\source.c src\markdown.c src\image.c
set CFLAGS=/nologo /std:c11 /O2 /W3 /MD /Ithird_party /Ithird_party\glfw\include /Isrc /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE
set LDFLAGS=/link /STACK:8388608 /LIBPATH:third_party\glfw\build\src\Release
set LIBS=glfw3.lib opengl32.lib gdi32.lib user32.lib shell32.lib

echo Compiling utterance...
cl %CFLAGS% %SRC% /Fe:ut.exe %LDFLAGS% %LIBS%

if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)

:: Clean up .obj files from current directory
del *.obj >nul 2>&1

echo.
echo Build complete: ut.exe
