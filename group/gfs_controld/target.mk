
$(eval $(call make-binary,/usr/sbin/gfs_controld,group/libgfscontrol/libgfscontrol.a -llogthread -lcpg -lpthread -lccs -lfenced -lcfg -ldlmcontrol -lcman -lquorum))

