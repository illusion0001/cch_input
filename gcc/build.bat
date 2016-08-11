@set src=%CD%\..
@mkdir %out%

@set log=build.log

@echo compile sources
@windres %src%\ffmpeg.rc %out%\rc.o 2>%log%

@set param=-I%src% -I%include% -D__STDC_CONSTANT_MACROS -Wno-deprecated-declarations -Wno-multichar -O2

@g++ -c %param% %src%\audiosource2.cpp -o %out%\audiosource2.o 2>>%log%
@g++ -c %param% %src%\cineform.cpp -o %out%\cineform.o 2>>%log%
@g++ -c %param% %src%\export.cpp -o %out%\export.o 2>>%log%
@g++ -c %param% %src%\fflayer.cpp -o %out%\fflayer.o 2>>%log%
@g++ -c %param% %src%\fflayer_render.cpp -o %out%\fflayer_render.o 2>>%log%
@g++ -c %param% %src%\fileinfo2.cpp -o %out%\fileinfo2.o 2>>%log%
@g++ -c %param% %src%\inputfile2.cpp -o %out%\inputfile2.o 2>>%log%
@g++ -c %param% %src%\main2.cpp -o %out%\main2.o 2>>%log%
@g++ -c %param% %src%\vfmain.cpp -o %out%\vfmain.o 2>>%log%
@g++ -c %param% %src%\videosource2.cpp -o %out%\videosource2.o 2>>%log%

@g++ -c %param% %src%\vd2\vdxframe\videofilter.cpp -o %out%\videofilter.o 2>>%log%
@g++ -c %param% %src%\vd2\vdxframe\videofilterdialog.cpp -o %out%\videofilterdialog.o 2>>%log%
@g++ -c %param% %src%\vd2\vdxframe\videofilterentry.cpp -o %out%\videofilterentry.o 2>>%log%

@set objlist=%out%\rc.o %out%\audiosource2.o %out%\cineform.o %out%\export.o %out%\fflayer.o %out%\fflayer_render.o %out%\fileinfo2.o %out%\inputfile2.o %out%\main2.o %out%\vfmain.o %out%\videofilter.o %out%\videofilterdialog.o %out%\videofilterentry.o %out%\videosource2.o
@set fflib=-lavcodec -lavformat -lavutil -lswresample -lswscale -lmp3lame -lavcodec
@set syslib=-lvfw32 -lcomdlg32 -lz -lopus -lvorbis -lvorbisenc -llzma -liconv -lbz2 -lws2_32 -lgmp -lsecur32 -logg

@echo linking
@g++ -O2 -static-libgcc -mdll -L%lib% %objlist% %fflib% %syslib% -o %out%\cch_input.vdf 2>>%log%
