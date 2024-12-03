add_rules("mode.debug", "mode.release")
add_requireconfs("*", {configs = {shared = false}, system=false})

add_requires("libusb")
add_packages("libusb")


package("rtlsdr_real")
    add_deps("cmake")
    set_sourcedir(path.join(os.scriptdir(), "deps/rtl-sdr"))
    
    on_install(function (package)
        local configs = {}
        table.insert(configs, "-DCMAKE_BUILD_TYPE=" .. (package:debug() and "Debug" or "Release"))
        table.insert(configs, "-DBUILD_SHARED_LIBS=" .. (package:config("shared") and "ON" or "OFF"))
        import("package.tools.cmake").install(package, configs)
    end)
    
    --[[
        on_install(function (package)
        local configs = {}
        table.insert(configs, "-DCMAKE_BUILD_TYPE=" .. (package:debug() and "Debug" or "Release"))
        table.insert(configs, "-DBUILD_SHARED_LIBS=" .. (package:config("shared") and "ON" or "OFF"))
        import("package.tools.cmake").install(package, configs)
    end)
    
    on_load(function (package)
        package:set("installdir", path.join(os.scriptdir(), package:plat(), package:arch(), package:mode()))
    end)
    
    on_fetch(function (package)
        local result = {}
        result.links = "rtlsdr.dll"
        result.linkdirs = package:installdir("lib")
        result.includedirs = package:installdir("include")
        return result
    end)
    --]]
    
package_end()

add_requires("rtlsdr_real", {configs={shared=true}})

add_includedirs("./deps")
--add_includedirs("./deps/rtl-sdr/include")
--add_linkdirs("deps/rtl-sdr/build/src")

set_languages("c99", "cxx23")

add_packages("libusb")
add_cxxflags("-fexperimental-library")

set_optimize("fastest")

if is_plat("windows", "mingw", "msys") then
    add_syslinks("ws2_32", "winmm")
else
    add_syslinks("rtlsdr")
end

if is_plat("msys", "mingw") then
    add_ldflags("-static -static-libgcc -static-libstdc++")
end

target("rtl_daemon")
    add_packages("rtlsdr_real")
    set_kind("binary")
    add_files("main.cpp")
    
target("rtlsdr")
    set_kind("shared")
    add_files("librtlsdr/main.cpp")