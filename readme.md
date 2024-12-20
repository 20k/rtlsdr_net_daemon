# Installation

1. Overwrite (backup first!) the old rtlsdr.dll/librtlsdr.dll bundled with an application, with the dlls provided in the latest release. You can find these under `dll`.
2. Start up the daemon rtlsdr_daemon.exe.
3. That's it, everything should work!

Both a 32-bit, and 64-bit replacement dll are provided (rtlsdr.dll, and librtlsdr.dll respectively). You'll need to replace the dll's for every application that you want to support via this method.

# Description

I was tired of only one application being able to use an rtlsdr at a time, so this is a drop in library replacement that automatically enables sharing of your rtlsdr between multiple applications - both control (eg setting the frequency), and reading data. Any application using the custom DLL will automagically work. There is also support for hot swapping of the underlying device: You may shut down and restart the daemon while an application is running, and there is even some very limited persistence of rtlsdr state (bandwidth, frequency, gain, and sample rate) between reboots.

# Configuration

This application relies on opening a number of UDP ports: Firstly, a query port which is used to set and request information about a device. This defaults to 6960. Secondly, one broadcast port per rtlsdr is used, starting at 6961 and incrementing upwards with every device (eg 6961, 6962, 6963 if you have three).

rtlsdr_daemon accepts a `config.json` file that looks as follows:

```json
{
    "root_device_port":6961,
    "query_port":6960,
    "persistence":true
}
```

The root device port may be freely modified. If you want to change the query port, you'll also need to add a configuration file in the same folder as your application's to configure the custom DLL.

This is called `librtlsdr_udp_config.json` which looks as follows:

```json
{
    "query_port":6960,
    "retry":true
}
```

# Compatibility

I've tested this with sdr++ and sdr#, as well as custom applications. Sharing devices works better than I expected, but applications aren't really built to have things like frequency or gain swapped out from under their feet, so there may be some UI problems here and there.

This application was only designed for sharing control on the same computer, and was not designed to be directly exposed to the internet. I wouldn't recommend it, as it uses UDP and will likely drop commands, as well as generally being unsafe - rtl_tcp is likely a better bet there, or building a custom web frontend. Give me a shout if you have a use case for not using broadcast ports.

# Support

All functions are supported, with the exception of eeprom read/write. 

Currently, only windows is supported: It should require virtually zero porting to linux, but I simply don't have a test environment. PRs are welcome! Multiple device support is also completely untested, but works in theory.

# Limitations/Implementation notes

Function error codes are generally not implemented, as it is more difficult to implement them performantly.

Async reads do not respect the `buf_num` parameter, as it makes little sense here.

There is no support for any kind of dynamic hotplugging while the daemon is running (it must be restarted), as it only polls and opens devices once.

`rtlsdr_reset_buffer` does nothing.

If the daemon is stopped, and devices are unplugged and shuffled around, when restarting persistence is simply done by device index. This may result in unexpected behaviour, and there's not much I can do about it. Set `"persistence":false` if this is a problem. If using custom serials is common practice with multiple rtlsdrs, please let me know and I'll add support.

Externally pausing the daemon may cause applications to falsely re-send requests for information, resulting in unexpected behaviour when it resumes processing old requests. If you want to disable the retry behaviour (and disable the ability to restart the daemon while applications are running), set `"retry":false` in the configuration for `librtlsdr_udp_config.json`.

Eeprom read/write is not supported.

# Development

As-is currently this project and meets everything I needed from building it, so please create an issue if you want additional features, as there's nothing on the todo list currently. If you find a bug, please report it or file a PR, and I'll fix it.

Linux support is something I'd like to add, and performance on low power hardware is completely untested and could likely be improved.

# Building

This project uses xmake via msys2 (specifically ucrt64) to compile on windows. Simply run `xmake` to build.

This was designed to be as easy to build as possible. The only dependencies are nlohmann::json (provided), and for the daemon: additionally librtlsdr, and libusb. Librtlsdr is added as a submodule here, so checkout with submodules if you want to build with the default xmake configuration.