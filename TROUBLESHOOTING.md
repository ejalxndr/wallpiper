# Troubleshooting

## SMPTE-(EG)-1-1990 on video based wallpapers

<img height="200" alt="image" src="https://github.com/user-attachments/assets/46f9a6cf-0991-4a9d-bfaf-4d29b7afce57" />

Wine's `mfplat` falls back to rendering (EG) 1-1990 when a proprietary codec is not supported. 
This is outside the scope of `wallpiper`. Many existing proton based compatibility layers have fixes builtin.

For instance:

* [GE-Proton11-5-x86_64](https://github.com/GloriousEggroll/proton-ge-custom/releases/tag/GE-Proton11-5) (`winegstreamer` is the key library)

## Black Screen/Can't open Wallpaper Engine

Likely an issue with your underling translation layer. Only GE-Proton-11 and Valve-Proton-11 have been tested working. 

All users experiencing this issue fixed it upgrading to a later version of Proton.  

## Position or Rotation Data of monitors is not accurately reflected

<img width="477" height="197" alt="image" src="https://github.com/user-attachments/assets/a3622567-0d59-450a-a736-e32bc7315455" />

By default, Wallpaper Engine utilizes the device path to identify the position and rotation data of monitors. Setting your monitor identification method to "Shell API" under Settings - General - Windows from within Wallpaper Engine may improve identification.

## GStreamer-WARNING: libbz2.so.1.0: No such file or directory (Fedora)

This error is Fedora-specific and is only seen at runtime, causing intermittent crashes in Wallpaper Engine even when idle. It can be fixed by creating a symbolic link for the critical library `libbz2.so.1.0`, as it is named `libbz2.so.1` in Fedora.

On standard Fedora distributions:

```
ln -s /usr/lib64/libbz2.so.1 /usr/lib64/libbz2.so.1.0
```

Immutable Fedora distributions (Fedora Silverblue, Aurora, Bazzite, etc.) will have to link this library to a user-accessible location. See [here](https://github.com/ublue-os/bazzite/issues/4978#issuecomment-4566369555) for an example of how to achieve this.

It is also worth ensuring GStreamer plugins are correctly installed. Check Freedesktop's installation instructions [here](https://gstreamer.freedesktop.org/documentation/installing/on-linux.html?gi-language=c).
