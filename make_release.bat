mkdir release
cd release
mkdir daemon
mkdir dll
cd ..
cp bin/release/rtl_daemon.exe release/daemon
cp librtlsdr/bin/release/librtlsdr.dll release/dll
cp librtlsdr/bin/release/rtlsdr.dll release/dll
rm release/daemon/save.json