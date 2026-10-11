
IOP_BIN = ../bdm_udp_fs_assault.irx


#bdm
BDM_OBJS = main.o bdm.o part_driver.o part_driver_mbr.o part_driver_gpt.o
IOP_LIBS += ../libbdm/lib/libbdm.a
IOP_CFLAGS += -I../libbdm/include/ -Wno-strict-aliasing

#bdmfs_fatfs
BDMFS_FATFS_OBJS = ff.o ffsystem.o ffunicode.o fs_driver.o diskio.o main.o udpfs_stub.o
IOP_LIBS += -static-libgcc -lgcc

#net (dev9 + smap + ministack + xfer + udptty, derived from UDPBD in the
# Neutrino repository). Only the hardware/network layer is used: the UDPBD
# block protocol is not included, UDPFS runs on top of it.
NET_OBJS = net_dev9.o net_smap.o net_ministack.o net_xfer.o net_udptty.o net_udpfs_client.o net_udpfs_config.o
IOP_CFLAGS += -mno-check-zero-division
IOP_CFLAGS += -DDEV9_PSX_SUPPORT=1 -DDEV9_GAMESTAR_WORKAROUND=1
IOP_CFLAGS += -Isrc/net/dev9/include/ -Isrc/net/include/ -Isrc/net/

IOP_OBJS = $(addprefix bdm_,$(BDM_OBJS)) $(addprefix bdmfs_,$(BDMFS_FATFS_OBJS)) $(NET_OBJS) imports.o exports.o
# The UDP tty (udptty_init) closes descriptors 0 and 1, replaces the
# "tty" driver and broadcasts everything the other modules write to
# stdout; if one of its open() calls fails it stays in a while(1) inside the
# SMAP thread. Debug builds only.
ifneq ($(DEBUG), 1)
  IOP_CFLAGS += -DNO_TTY
endif
# UDPFS client knob:
#   make UDPFS_STATS=1   live statistics during the game (one line per
#                        second in the server log in -verbose, plus the
#                        reads not aligned to 512 bytes)
# After changing a knob: make clean (make does not see the -D flags).
ifeq ($(UDPFS_STATS), 1)
  IOP_CFLAGS += -DUDPFS_STATS
endif
ifeq ($(DEBUG), 1)
  IOP_CFLAGS += -DDEBUG -DDEBUG_EXTREME
  IOP_BIN = ../bdm_udp_fs_assault_debug.irx
endif
include ../global.make
IOP_SRC_DIR =
vpath %.c src/
vpath %.lst src/
vpath %.tab src/
vpath %.c src/bdm/
vpath %.c src/bdmfs/
vpath %.c src/net/


$(IOP_OBJS_DIR)%.o: src/bdm/%.c
	$(DIR_GUARD)
	$(IOP_C_COMPILE) -c $< -o $@
$(IOP_OBJS_DIR)%.o: src/bdmfs/%.c
	$(DIR_GUARD)
	$(IOP_C_COMPILE) -c $< -o $@
$(IOP_OBJS_DIR)%.o: src/net/%.c
	$(DIR_GUARD)
	$(IOP_C_COMPILE) -c $< -o $@