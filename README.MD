# BDM Assault

BDM Assault is a homebrew project for PS2 that aims to bring EXFAT USB support to those old closed source homebrew apps that can load external USB drivers that people still use today (POPStarter, FreeMcBoot).

## BDM-UDPFS Assault (fork)

This repository is a fork of [israpps/BDMAssault](https://github.com/israpps/BDMAssault). On top of the original project it adds [`__bdm_udp_fs_assault`](__bdm_udp_fs_assault/README.MD): a variant of the BDM driver for POPStarter that reads `.VCD` files from a UDPFS server over Ethernet when they are not found on the USB drive (read-only, network fallback). A second build (`make EMBER=1`) does the same for the Ember emulator (`.cue`/`.bin` files).

- Output: `bdm_udp_fs_assault.irx`, a drop-in replacement for `bdm_assault.irx` (paste it into `mc?:/POPSTARTER/usbd.irx`); `bdm_udp_fs_assault_ember.irx` for Ember.
- It works with the same `usbd_bd_assault.irx` built from this repository.
- The original BDM Assault is unchanged and keeps working as described below.
- Usage, network configuration and build options: see [`__bdm_udp_fs_assault/README.MD`](__bdm_udp_fs_assault/README.MD).

## License

This project is distributed under the terms found in the [LICENSE](LICENSE) file.
The UDPFS variant also includes or derives from third-party works that keep their own licenses (PS2SDK, Neutrino/UDPBD/UDPFS, FatFs, udpfsd). The full list, with copyright notices and license texts, is in [`__bdm_udp_fs_assault/docs/THIRD_PARTY_NOTICES.md`](__bdm_udp_fs_assault/docs/THIRD_PARTY_NOTICES.md) and [`__bdm_udp_fs_assault/licenses/`](__bdm_udp_fs_assault/licenses/).

## Usage

To use this project you have to place the two IRX binaries provided at the [release section](https://github.com/israpps/BDMAssault/releases) into the path that your app looks for


### POPStarter
- `bdm_assault.irx` must be pasted into `mc?:/POPSTARTER/usbd.irx`
- `usbd_bd_assault.irx` must be pasted into `mc?:/POPSTARTER/usbhdfsd.irx`

### FreeMcBoot
- `bdm_assault.irx` must be pasted into `mc?:/SYS-CONF/USBD.IRX`
- `usbd_bd_assault.irx` must be pasted into `mc?:/SYS-CONF/USBHDFSD.IRX`

### FreeHdBoot
- `bdm_assault.irx` must be pasted into `hdd0:__sysconf/FMCB/USBD.IRX`
- `usbd_bd_assault.irx` must be pasted into `hdd0:__sysconf/FMCB/USBHDFSD.IRX`

### old wLaunchELF versions
- `bdm_assault.irx` must be pasted into the path you configured for `USBD.IRX`
- `usbd_bd_assault.irx` must be pasted into the path you configured for `USBHDFSD.IRX`

### Any other homebrew
- find out by yourself. I cant remember any other.

## Incompatible programs

- Simple Media System (SMS): incompatible due to usage of custom USB drivers and practices wich are not common amongst the homebrew devs. support not planned (and most likely not even possible)

## CREDITS

the softwares found here were taken and modified from [__PS2SDK__](https://github.com/ps2dev/ps2sdk)

- krHACKen: for letting me know POPStarter loads external USB drivers (wich ended up in the creation of this project)
- juankytutoriales: for making tests in real hardware
- everyone that collaborated to PS2SDK
- ps2max32 ([Neutrino](https://github.com/ps2max32/neutrino)): UDPFS/UDPBD/UDPRDMA protocol and network code used by the UDPFS variant
- PCM720 ([udpfsd](https://github.com/pcm720/udpfsd)): UDPFS server in Go, used as the reference for the client