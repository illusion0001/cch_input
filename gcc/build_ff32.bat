@set msys=E:\work\ffmpeg_build\msys64
@set bin=%msys%\mingw32\bin
@path %path%;%bin%;%msys%\usr\bin

@set dir=%CD%
@cd E:\work\ffmpeg_build\build\ffmpeg-git\build-static-32bit

@make >%dir%\buildff.log

@cd %dir%
