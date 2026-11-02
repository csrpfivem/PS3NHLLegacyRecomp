@echo off
rem Sets up MSVC headers/libs (vcvars64) plus the local LLVM toolchain, then runs the given command.
rem Usage: devenv.cmd <command> [args...]
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PORT_WORK=%~dp0.."
set "PATH=%PORT_WORK%\toolchain\clang+llvm-23.1.3-x86_64-pc-windows-msvc\bin;%PATH%"
%*
