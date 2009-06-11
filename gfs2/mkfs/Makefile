TARGET1= mkfs.gfs2
TARGET2= gfs2_jadd
TARGET3= gfs2_grow

FORCESBINT= $(TARGET1)

SBINSYMT=$(TARGET2) $(TARGET3)

all: depends ${TARGET1} ${TARGET2} ${TARGET3}

include ../../make/defines.mk
include $(OBJDIR)/make/cobj.mk
include $(OBJDIR)/make/clean.mk
include $(OBJDIR)/make/install.mk
include $(OBJDIR)/make/uninstall.mk

OBJS=	main.o \
	main_mkfs.o \
	main_grow.o \
	main_jadd.o

CFLAGS += -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE -D_GNU_SOURCE
CFLAGS += -I${KERNEL_SRC}/include/
CFLAGS += -I$(S)/../include -I$(S)/../libgfs2
CFLAGS += -I${incdir}

LDFLAGS += -L../libgfs2 -lgfs2
LDFLAGS += -L${libdir}

LDDEPS += ../libgfs2/libgfs2.a

${TARGET1}: ${OBJS} ${LDDEPS}
	$(CC) -o $@ $^ $(LDFLAGS)

${TARGET2}: ${TARGET1}
	ln -sf ${relativesbin}/${TARGET1} ${TARGET2}

${TARGET3}: ${TARGET1}
	ln -sf ${relativesbin}/${TARGET1} ${TARGET3}

depends:
	$(MAKE) -C ../libgfs2 all

clean: generalclean

-include $(OBJS:.o=.d)
