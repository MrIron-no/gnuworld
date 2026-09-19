## Process this file with automake to produce Makefile.in
## $Id: Makefile.am,v 1.9 2004/01/07 20:23:20 dan_karrels Exp $

noinst_LTLIBRARIES += libnotifier.la
libnotifier_la_SOURCES = \
	libnotifier/pushover.cc

if COND_PROMETHEUS
libnotifier_la_SOURCES += libnotifier/prometheus.cc
endif

# Core logs through the logger: the global elog is not declared here (see libgnuworld/ELog.h)
libnotifier_la_CXXFLAGS = \
	-DGNUWORLD_NO_ELOG \
	-I$(top_srcdir)/include \
	-I$(top_srcdir)/libgnuworld \
	-I$(top_srcdir)/libnotifier

libnotifier_la_LIBADD =

if COND_LIBCURL
if COND_PROMETHEUS
libnotifier_la_LIBADD += -lcurl -lprometheus-cpp-core -lprometheus-cpp-pull
else
libnotifier_la_LIBADD += -lcurl
endif
else
if COND_PROMETHEUS
libnotifier_la_LIBADD += -lprometheus-cpp-core -lprometheus-cpp-pull
endif
endif

EXTRA_DIST += \
	libnotifier/notifier.h \
	libnotifier/prometheus.h \
	libnotifier/pushover.h
