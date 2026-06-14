@echo off

:top
shift
if "%0" equ "" goto :eof
if "%0" equ "--cc" goto cc
if "%0" equ "--cflags" goto cflags
if "%0" equ "--ld" goto ld
if "%0" equ "--ldflags" goto ldflags
if "%0" equ "--ldflags-before-libs" goto ldflagsbeforelibs
if "%0" equ "--libs" goto libs
if "%0" equ "--libmruby-path" goto libmrubypath
if "%0" equ "--help" goto showhelp
echo Invalid Option
goto :eof

:cc
echo gcc
goto top

:cflags
echo -std=gnu99 -g -O3 -Wall -Wundef -Werror-implicit-function-declaration -Wwrite-strings -DMRB_USE_RATIONAL -DMRB_USE_COMPLEX -DMRB_USE_BIGINT -I"/home/USER/mruby32/include" -I"/home/USER/mruby32/build/host/include" -I"/home/USER/mruby32/mrbgems/mruby-time/include" -I"/home/USER/mruby32/mrbgems/mruby-io/include"
goto top

:ld
echo gcc
goto top

:libs
echo -lmruby 
goto top

:ldflags
echo  -L/home/USER/mruby32/build/host/lib
goto top

:ldflagsbeforelibs

goto top

:libmrubypath
echo /home/USER/mruby32/build/host/lib/libmruby.a
goto top

:showhelp
echo Usage: mruby-config [switches]
echo   switches:
echo   --cc                       print compiler name
echo   --cflags                   print flags passed to compiler
echo   --ld                       print linker name
echo   --ldflags                  print flags passed to linker
echo   --ldflags-before-libs      print flags passed to linker before linked libraries
echo   --libs                     print linked libraries
echo   --libmruby-path            print libmruby path
