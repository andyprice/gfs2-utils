uninstall:
ifdef LIBDIRT
	${UNINSTALL} ${LIBDIRT} ${libdir}
endif
ifdef LIBSYMT
	${UNINSTALL} ${LIBSYMT} ${libdir}
endif
ifdef INCDIRT
	${UNINSTALL} ${INCDIRT} ${incdir}
endif
ifdef SBINDIRT
	${UNINSTALL} ${SBINDIRT} ${sbindir}
endif
ifdef SBINSYMT
	${UNINSTALL} ${SBINSYMT} ${sbindir}
endif
ifdef INITDT
	${UNINSTALL} ${INITDT} ${initddir}
endif
ifdef DOCS
	${UNINSTALL} ${DOCS} ${docdir}
endif
