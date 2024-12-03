add_rules("mode.debug", "mode.release")
add_requireconfs("*", {configs = {shared = false}, system=false})

add_requires("libusb")

add_includedirs("./deps")
add_includedirs("./deps/rtl-sdr/include")
add_linkdirs("deps/rtl-sdr/build/src")

set_languages("c99", "cxx23")

add_packages("libusb")
add_cxxflags("-fexperimental-library")

set_optimize("fastest")

if is_plat("windows", "mingw", "msys") then
    add_syslinks("ws2_32", "winmm", "rtlsdr.dll")
else
    add_syslinks("rtlsdr")
end

if is_plat("msys", "mingw") then
    add_ldflags("-static -static-libgcc -static-libstdc++")
end

target("rtl_daemon")
    set_kind("binary")
    add_files("main.cpp")
    
target("rtlsdr")
    set_kind("shared")
    add_files("librtlsdr/main.cpp")