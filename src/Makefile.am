## Process this file with automake to produce Makefile.in

yeaphone_CPPFLAGS = @LINPHONE_CFLAGS@

# Program binary name
bin_PROGRAMS = yeaphone

# sources 
yeaphone_SOURCES = lpcontrol.c  yeaphone.c ylcontrol.h yldisp.h ypconfig.h \
                   lpcontrol.h  ylcontrol.c  yldisp.c ypconfig.c \
                   ypmainloop.h ypmainloop.c ylsysfs.h ylsysfs.c

# libraries
yeaphone_LDADD = @LINPHONE_LIBS@
yeaphone_LDFLAGS = -Wl,--rpath -Wl,@LINPHONE_LIBDIR@ @LIBTHREAD@

# mark headers to include also in package
#EXTRA_DIST = talk.h
