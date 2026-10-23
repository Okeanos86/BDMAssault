IOP_BIN = ../udp_fs_assault.irx

# UDP FS Assault: the "mass" device served by a UDPFS server, with read and
# write support. No block devices, no FatFs, no USB. Only the hardware and
# network layer (dev9 + smap + ministack, derived from UDPBD in the Neutrino
# repository) and the UDPFS client.
UDPFS_OBJS = udpfs.o udpfs_main.o udpfs_bdm_stub.o
IOP_LIBS += -static-libgcc -lgcc

NET_OBJS = net_dev9.o net_smap.o net_ministack.o net_xfer.o net_udptty.o net_udpfs_client.o net_udpfs_config.o net_udpfs_stash.o
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
include ../global.make
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
