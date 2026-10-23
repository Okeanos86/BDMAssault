# UDPFS Assault

UDPFS Assault is a PS2 IOP module that makes a **UDPFS server** look like the `mass:` device, with **read and write** support. Programs that expect a USB drive on `mass:` (POPStarter, Ember) run with their files served over the network.

This branch is a fork of [israpps/BDMAssault](https://github.com/israpps/BDMAssault). The module lives in [`__udp_fs_assault`](__udp_fs_assault/README.MD).

- Output: `udp_fs_assault.irx`, a drop-in replacement for `bdm_assault.irx` (paste it into `mc?:/POPSTARTER/usbd.irx`).
- Tested on hardware with POPStarter (`.VCD` + `SLOT0/1.VMC`) and Ember (`.cue`/`.bin` + `MC1/MC2.vmc`), against `udpfsd` (Go) and `udpfs_server.py` (Python).
- Network configuration, server requirements, build options and **warnings about writing over the network**: see [`__udp_fs_assault/README.MD`](__udp_fs_assault/README.MD) and [`__udp_fs_assault/docs/README_CONFIG.txt`](__udp_fs_assault/docs/README_CONFIG.txt).

## BDM Assault (original project)

BDM Assault is a homebrew project for PS2 that aims to bring EXFAT USB support to those old closed source homebrew apps that can load external USB drivers that people still use today (POPStarter, FreeMcBoot).

## License

This project is distributed under the terms found in the [LICENSE](LICENSE) file.
UDPFS Assault also include or derive from third-party works that keep their own licenses (PS2SDK, Neutrino/UDPBD/UDPFS, udpfsd. The full lists, with copyright notices and license texts, are in [`__udp_fs_assault/docs/THIRD_PARTY_NOTICES.md`](__udp_fs_assault/docs/THIRD_PARTY_NOTICES.md) and the `licenses/` folders.

## Usage

To use the original BDM Assault you have to place the two IRX binaries provided at the [release section](https://github.com/israpps/BDMAssault/releases) into the path that your app looks for.
For UDP FS Assault, `udp_fs_assault.irx` takes the place of `bdm_assault.irx` and the other file is still `usbd_bd_assault.irx` (POPStarter does not run without it).

### POPStarter
- `bdm_assault.irx` (or `udp_fs_assault.irx`) must be pasted into `mc?:/POPSTARTER/usbd.irx`
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

### Ember
- `udp_fs_assault.irx` must be loaded by a launcher; see the module README.

### Any other homebrew
- find out by yourself. I cant remember any other.

## Incompatible programs

- Simple Media System (SMS): incompatible due to usage of custom USB drivers and practices wich are not common amongst the homebrew devs. support not planned (and most likely not even possible)

## CREDITS

the softwares found here were taken and modified from [__PS2SDK__](https://github.com/ps2dev/ps2sdk)

- krHACKen: for letting me know POPStarter loads external USB drivers (wich ended up in the creation of this project)
- juankytutoriales: for making tests in real hardware
- everyone that collaborated to PS2SDK
- ps2max32 ([Neutrino](https://github.com/ps2max32/neutrino)): UDPFS/UDPBD/UDPRDMA protocol and network code used by UDPFS Assault
- PCM720 ([udpfsd](https://github.com/pcm720/udpfsd)): UDPFS server in Go, used as the reference for the client
