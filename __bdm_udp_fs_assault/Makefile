
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
NET_OBJS = net_dev9.o net_smap.o net_ministack.o net_xfer.o net_udptty.o net_udpfs_client.o net_udpfs_config.o net_udpfs_stash.o
IOP_CFLAGS += -mno-check-zero-division
IOP_CFLAGS += -DDEV9_PSX_SUPPORT=1 -DDEV9_GAMESTAR_WORKAROUND=1
IOP_CFLAGS += -Isrc/net/dev9/include/ -Isrc/net/include/ -Isrc/net/

IOP_OBJS = $(addprefix bdm_,$(BDM_OBJS)) $(addprefix bdmfs_,$(BDMFS_FATFS_OBJS)) $(NET_OBJS) imports.o exports.o
# The UDP tty (udptty_init) closes descriptors 0 and 1, replaces the
# "tty" driver and broadcasts everything the other modules write to
# stdout; if one of its open() calls fails it stays in a while(1) inside the
# SMAP thread. Debug builds only.
#   make TTY=1           release build (no DEBUG traces) with the UDP tty on: the
#                        "UDPFS: ..." lines of the client (discovery, open, request
#                        failures) are broadcast on UDP port 18194 (nc -u -l 18194)
ifneq ($(DEBUG), 1)
  ifneq ($(TTY), 1)
    IOP_CFLAGS += -DNO_TTY
  endif
endif
# After changing a knob (EMBER, DEBUG, TTY): make clean (make does not see the -D flags).
# make clean removes the objects and every output variant.
# Ember build:
#   make EMBER=1         for the Ember emulator (a single module instance, no
#                        IOP reset): the config is read from the memory card
#                        without the IOP RAM carry-over. Output
#                        bdm_udp_fs_assault_ember.irx. The default build is
#                        the POPStarter one.
ifeq ($(EMBER), 1)
  IOP_CFLAGS += -DEMBER_BUILD
  IOP_BIN = ../bdm_udp_fs_assault_ember.irx
endif
ifeq ($(DEBUG), 1)
  # net_dbg.c logs how POPStarter loads the module (arguments)
  IOP_OBJS += net_dbg.o
  IOP_CFLAGS += -DDEBUG -DDEBUG_EXTREME
  ifeq ($(EMBER), 1)
    IOP_BIN = ../bdm_udp_fs_assault_ember_debug.irx
  else
    IOP_BIN = ../bdm_udp_fs_assault_debug.irx
  endif
endif
include ../global.make

# "make clean" removes only the IRX of the selected build; remove every variant
# (and the intermediate .elf files) so that nothing stale stays in the repo root.
clean: clean-variants
clean-variants:
	rm -f ../bdm_udp_fs_assault*.irx ../bdm_udp_fs_assault*.elf
.PHONY: clean-variants
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