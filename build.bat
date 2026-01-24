@echo off
echo !! Begin build
gcc -v -O3 winprecision-drawing.c -lhid -lgdi32 -o binary.exe
echo !! End build
pause