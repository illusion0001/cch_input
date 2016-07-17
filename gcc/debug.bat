@set bin=e:\work\mingw\bin
@path "%path%;%bin%"

%bin%\gdb --args E:\work\vd_filterMod\out\Debug\VirtualDub.exe /F %CD%\obj32\cch_input.vdf E:\shift_dev\data\test\mercalli.mp4 
