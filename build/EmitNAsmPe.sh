type sample.psd | parashade.exe --emit-nasm .out
REM Then build native PE directly from assembly:
cd .out
build.bat
