TARGET= mount.gfs2

FORCESBINT= $(TARGET)

all: ${TARGET}

include ../../make/defines.mk
include $(OBJDIR)/make/cobj.mk
include $(OBJDIR)/make/clean.mk
include $(OBJDIR)/make/install.mk
include $(OBJDIR)/make/uninstall.mk

OBJS=	mount.gfs2.o \
	ondisk1.o \
	util.o \
	mtab.o

CFLAGS += -DHELPER_PROGRAM -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE
CFLAGS += -I${KERNEL_SRC}/fs/gfs2/ -I${KERNEL_SRC}/include/
CFLAGS += -I$(SRCDIR)/group/libgfscontrol
CFLAGS += -I$(S)/../include
CFLAGS += -I${incdir}

LDFLAGS += -L../../group/libgfscontrol -lgfscontrol
LDFLAGS += -L${libdir}

LDDEPS += ../../group/libgfscontrol/libgfscontrol.a

${TARGET}: ${OBJS} ${LDDEPS}
	$(CC) -o $@ $^ $(LDFLAGS)

clean: generalclean

-include $(OBJS:.o=.d)
