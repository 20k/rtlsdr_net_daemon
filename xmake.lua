add_rules("mode.debug", "mode.release")
add_requireconfs("*", {configs = {shared = false}, system=false})

add_requires("libusb")
add_packages("libusb")

add_includedirs("./deps")
add_includedirs("./deps/rtl-sdr/include")

set_languages("c99", "cxx23")

add_packages("libusb")
add_cxxflags("-fexperimental-library")

set_optimize("fastest")

if is_plat("windows", "mingw", "msys") then
    add_syslinks("ws2_32", "winmm")
end

if is_plat("msys", "mingw") then
    add_ldflags("-static -static-libgcc -static-libstdc++")
end

target("rtlsdr_daemon")
    set_kind("binary")
    add_files("deps/rtl-sdr/src/tuner_e4k.c")
    add_files("deps/rtl-sdr/src/tuner_fc0012.c")
    add_files("deps/rtl-sdr/src/tuner_fc0013.c")
    add_files("deps/rtl-sdr/src/tuner_fc2580.c")
    add_files("deps/rtl-sdr/src/tuner_r82xx.c")
    add_files("deps/rtl-sdr/src/librtlsdr.c")
    add_files("main.cpp")
    
target("rtlsdr")
    set_kind("shared")
    add_files("librtlsdr/main.cpp")