IOP_BIN = ../udp_fs_assault.irx

# UDP FS Assault: the "mass" device served by a UDPFS server, with read and
# write support. No block devices, no FatFs, no USB. Only the hardware and
# network layer (dev9 + smap + ministack, derived from UDPBD in the Neutrino
# repository) and the UDPFS client.
# vcd_cue.o: a .cue + .bin set served as a POPStarter VCD. The largest cue
# sheet (VCD_CUE_MAX) and the room for the file names (VCD_NAMES_MAX) are set in
# src/udpfs/vcd_cue.h, the same values as in __bdm_udp_fs_assault (about 46 KB
# of static IOP RAM).
UDPFS_OBJS = udpfs.o vcd_cue.o udpfs_main.o udpfs_bdm_stub.o
IOP_LIBS += -static-libgcc -lgcc

NET_OBJS = net_dev9.o net_smap.o net_ministack.o net_xfer.o net_udptty.o net_udpfs_client.o net_udpfs_config.o net_udpfs_stash.o
# ../libbdm/include is only for bdm.h (the empty bdm library the stub exports)
IOP_CFLAGS += -mno-check-zero-division -Wno-strict-aliasing -I../libbdm/include/
IOP_CFLAGS += -DDEV9_PSX_SUPPORT=1 -DDEV9_GAMESTAR_WORKAROUND=1
IOP_CFLAGS += -Isrc/net/dev9/include/ -Isrc/net/include/ -Isrc/net/ -Isrc/udpfs/

IOP_OBJS = $(UDPFS_OBJS) $(NET_OBJS) imports.o exports.o

# The UDP tty (udptty_init) closes descriptors 0 and 1, replaces the "tty"
# driver and broadcasts everything the other modules write to stdout; if one
# of its open() calls fails it stays in a while(1) inside the SMAP thread.
# Debug builds only.
ifneq ($(DEBUG), 1)
  IOP_CFLAGS += -DNO_TTY
endif
ifeq ($(DEBUG), 1)
  # net_dbg.c logs how the module is loaded (arguments); udpfs.c logs
  # every operation with its result
  IOP_OBJS += net_dbg.o
  IOP_CFLAGS += -DDEBUG
  IOP_BIN = ../udp_fs_assault_debug.irx
endif
# After changing a knob (DEBUG): make clean (make does not see the -D flags).
include ../global.make

# "make clean" removes only the IRX of the selected build; remove every variant
# (and the intermediate .elf files) so that nothing stale stays in the repo root.
clean: clean-variants
clean-variants:
	rm -f ../udp_fs_assault*.irx ../udp_fs_assault*.elf
.PHONY: clean-variants
IOP_SRC_DIR =
vpath %.c src/
vpath %.lst src/
vpath %.tab src/
vpath %.c src/udpfs/
vpath %.c src/net/


$(IOP_OBJS_DIR)%.o: src/udpfs/%.c
	$(DIR_GUARD)
	$(IOP_C_COMPILE) -c $< -o $@
$(IOP_OBJS_DIR)%.o: src/net/%.c
	$(DIR_GUARD)
	$(IOP_C_COMPILE) -c $< -o $@
