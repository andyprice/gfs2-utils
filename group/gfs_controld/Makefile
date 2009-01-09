TARGET= gfs_controld

SBINDIRT=$(TARGET)

all: ${TARGET}

include ../../make/defines.mk
include $(OBJDIR)/make/cobj.mk
include $(OBJDIR)/make/clean.mk
include $(OBJDIR)/make/install.mk
include $(OBJDIR)/make/uninstall.mk

OBJS= 	main.o \
	member_cman.o \
	config.o \
	crc.o \
	cpg-new.o \
	util.o \
	logging.o

CFLAGS += -I${ccsincdir} -I${cmanincdir} -I${logtincdir} -I${dlmcontrolincdir}
CFLAGS += -I${corosyncincdir} -I${openaisincdir} -I${fencedincdir}
CFLAGS += -I${KERNEL_SRC}/include/
CFLAGS += -I$(S)/../libgfscontrol 
CFLAGS += -I$(S)/../include/
CFLAGS += -I${incdir}

LDFLAGS += -L${ccslibdir} -lccs -L${cmanlibdir} -lcman
LDFLAGS += -L${dlmcontrollibdir} -ldlmcontrol
LDFLAGS += -L${logtlibdir} -llogthread
LDFLAGS += -L${corosynclibdir} -lcpg -lpthread
LDFLAGS += -L${openaislibdir} -lSaCkpt
LDFLAGS += -L${fencedlibdir} -lfenced
LDFLAGS += -L${libdir}

LDDEPS += ${fencedlibdir}/libfenced.a

${TARGET}: ${OBJS} ${LDDEPS}
	$(CC) -o $@ $^ $(LDFLAGS)

clean: generalclean

-include $(OBJS:.o=.d)
