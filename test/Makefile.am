## Process this file with automake to produce Makefile.in
## $Id: Makefile.am,v 1.19 2007/09/15 18:03:34 dan_karrels Exp $

noinst_PROGRAMS = test_burst \
	test_mtrie_load \
	test_mtrie_perf_summary \
	test_mtrie_perf \
	test_mtrie \
	test_stringtokenizer \
	test_econfig \
	test_gThread \
	test_signal \
	test_match \
	test_bot \
	test_kick_transaction \
	test_xparameters_tags \
	test_stringtokenizer_trailing \
	test_channelmodes \
	test_checkedformat \
	test_numbers \
	test_logger_render \
	test_logger_format \
	test_logger_sinks \
	test_logger_basic \
	test_logger_hierarchy \
	test_logger_config \
	test_elog_shim

if COND_PCRE
noinst_PROGRAMS += test_regex
endif

test_burst_SOURCES = test/burst.cc
test_burst_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld
test_burst_LDADD = libgnuworld.la

test_mtrie_SOURCES = test/mtrie.cc
test_mtrie_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld
test_mtrie_LDADD = libgnuworld.la

test_mtrie_load_SOURCES = test/mtrie_load.cc
test_mtrie_load_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld
test_mtrie_load_LDADD = libgnuworld.la

test_mtrie_perf_SOURCES = test/mtrie_perf.cc
test_mtrie_perf_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld
test_mtrie_perf_LDADD = libgnuworld.la

test_mtrie_perf_summary_SOURCES = test/mtrie_perf_summary.cc
test_mtrie_perf_summary_CXXFLAGS = -I${top_srcdir}/include \
	-I${top_srcdir}/libgnuworld
test_mtrie_perf_summary_LDADD = libgnuworld.la

test_stringtokenizer_SOURCES = test/stringtokenizer.cc
test_stringtokenizer_CXXFLAGS = -I${top_srcdir}/include \
	-I${top_srcdir}/libgnuworld
test_stringtokenizer_LDADD = libgnuworld.la

test_econfig_SOURCES = test/econfig.cc
test_econfig_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld
test_econfig_LDADD = libgnuworld.la

test_match_SOURCES = test/match.cc
test_match_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld
test_match_LDADD = libgnuworld.la

test_bot_SOURCES = test/testbot.cc
test_bot_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld
test_bot_LDADD = libgnuworld.la

test_kick_transaction_SOURCES = test/test_kick_transaction.cc
test_kick_transaction_CXXFLAGS = -I${top_srcdir}/include \
	-I${top_srcdir}/libgnuworld
test_kick_transaction_LDADD = libgnuworld.la

test_xparameters_tags_SOURCES = test/xparameters_tags.cc
test_xparameters_tags_CXXFLAGS = -I${top_srcdir}/include \
	-I${top_srcdir}/libgnuworld
test_xparameters_tags_LDADD = libgnuworld.la

test_stringtokenizer_trailing_SOURCES = test/stringtokenizer_trailing.cc
test_stringtokenizer_trailing_CXXFLAGS = -I${top_srcdir}/include \
	-I${top_srcdir}/libgnuworld
test_stringtokenizer_trailing_LDADD = libgnuworld.la

test_regex_SOURCES = test/test_regex.cc
test_regex_LDADD = libgnuworld.la -lpcre

test_gThread_SOURCES = test/gThread.cc
test_gThread_LDADD = libgnuworld.la $(threadLib)
test_gThread_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld

test_signal_SOURCES = test/signal.cc
test_signal_LDADD = libgnuworld.la $(threadLib)
test_signal_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld

EXTRA_DIST += \
	test/testbot.h \
	test/README.burst \
	test/README.econfig \
	test/README.gthread \
	test/README.match \
	test/README.mtrie \
	test/README.signal \
	test/README.stringtokenizer \
	test/README.testBot \
	test/README.test_kick_transaction

# The mode functions are Channel's, in src/Channel_modes.cc, which needs
# nothing but libgnuworld and so is built straight into the test.
test_channelmodes_SOURCES = test/channelmodes.cc src/Channel_modes.cc
test_channelmodes_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld \
	-I${top_srcdir}/libnotifier -I${top_srcdir}/db
test_channelmodes_LDADD = libgnuworld.la

test_checkedformat_SOURCES = test/checkedformat.cc
test_checkedformat_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld
test_checkedformat_LDADD = libgnuworld.la

test_numbers_SOURCES = test/numbers.cc
test_numbers_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld
test_numbers_LDADD = libgnuworld.la

test_logger_render_SOURCES = test/logger_render.cc
test_logger_render_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld
test_logger_render_LDADD = libgnuworld.la $(threadLib)

test_logger_format_SOURCES = test/logger_format.cc
test_logger_format_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld
test_logger_format_LDADD = libgnuworld.la $(threadLib)

test_logger_sinks_SOURCES = test/logger_sinks.cc
test_logger_sinks_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld
test_logger_sinks_LDADD = libgnuworld.la $(threadLib)

# libgnuworld.la and nothing else: the proof that the logger has no dependency
# on the core any more.  libnotifier is on the include path but not on the link
# line: notifier.h is the whole of the base class the notifier cases exercise,
# and it names nothing outside libgnuworld either.
test_logger_basic_SOURCES = test/logger_basic.cc
test_logger_basic_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld \
	-I${top_srcdir}/libnotifier
test_logger_basic_LDADD = libgnuworld.la $(threadLib)

# The registry, the hierarchy and the module names, on libgnuworld alone.
test_logger_hierarchy_SOURCES = test/logger_hierarchy.cc
test_logger_hierarchy_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld
test_logger_hierarchy_LDADD = libgnuworld.la $(threadLib)

# The logging.conf parser and the configuration apply, on libgnuworld alone.
# The example file that ships in bin/ is one of the files it parses, so that a
# mistake in the documentation of the syntax is a failing test.
test_logger_config_SOURCES = test/logger_config.cc
test_logger_config_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld \
	-DLOGGING_EXAMPLE_CONF=\"$(top_srcdir)/bin/logging.example.conf\"
test_logger_config_LDADD = libgnuworld.la $(threadLib)

# The deprecated elog in front of the logger, on libgnuworld alone.
test_elog_shim_SOURCES = test/elog_shim.cc
test_elog_shim_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld
test_elog_shim_LDADD = libgnuworld.la $(threadLib)

# Run by "make check".  The other programs in this directory are
# interactive or benchmarks, not tests.
TESTS = test_channelmodes test_checkedformat test_numbers test_logger_render \
	test_logger_format test_logger_sinks test_logger_basic test_logger_hierarchy \
	test_logger_config test_elog_shim
