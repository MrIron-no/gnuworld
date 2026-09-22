## Process this file with automake to produce Makefile.in
## $Id: Makefile.am,v 1.19 2007/09/15 18:03:34 dan_karrels Exp $

noinst_PROGRAMS = test_burst \
	test_mtrie_load \
	test_mtrie_perf_summary \
	test_mtrie_perf \
	test_mtrie \
	test_stringtokenizer \
	test_econfig \
	test_signal \
	test_match \
	test_bot \
	test_kick_transaction \
	test_xparameters_tags \
	test_stringtokenizer_trailing \
	test_channelmodes \
	test_channelbans \
	test_checkedformat \
	test_numbers \
	test_events_names \
	test_logger_render \
	test_logger_format \
	test_logger_sinks \
	test_logger_basic \
	test_logger_hierarchy \
	test_logger_config \
	test_logger_ratelimit \
	test_elog_shim \
	test_logger_adversarial \
	test_pushover_text

if COND_PCRE
noinst_PROGRAMS += test_regex
endif

# The pushover sink really talking to an HTTP endpoint, which needs the libcurl
# the sink is compiled against: where there is none there is no sink either, so
# there is nothing for this program to test (see test/docker/full-deps/run.sh,
# which is the build that has one).
if COND_LIBCURL
noinst_PROGRAMS += test_pushover_delivery
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

test_signal_SOURCES = test/signal.cc
test_signal_LDADD = libgnuworld.la $(threadLib)
test_signal_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld

EXTRA_DIST += \
	test/testbot.h \
	test/README.burst \
	test/README.econfig \
	test/README.match \
	test/README.mtrie \
	test/README.sanitize \
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

# The ban bookkeeping is Channel's too, but it lives in src/Channel.cc, which
# needs the network tables and the connection: this one links the whole core
# and builds a Channel of its own, no xServer running.
test_channelbans_SOURCES = test/channelbans.cc
test_channelbans_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld \
	-I${top_srcdir}/libnotifier -I${top_srcdir}/db
test_channelbans_LDADD = libgnuworldcore.la libgnuworld.la

test_checkedformat_SOURCES = test/checkedformat.cc
test_checkedformat_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld
test_checkedformat_LDADD = libgnuworld.la

test_numbers_SOURCES = test/numbers.cc
test_numbers_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld
test_numbers_LDADD = libgnuworld.la

# The event names of include/events.h, which is a header and nothing else: no
# library is linked at all.
test_events_names_SOURCES = test/events_names.cc
test_events_names_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld

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

# The token bucket a sink that talks to the outside world holds, on libgnuworld
# alone.  Every case that is about the refill injects the clock rather than
# sleeping, so this is as fast as any other unit test here.
test_logger_ratelimit_SOURCES = test/logger_ratelimit.cc
test_logger_ratelimit_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld
test_logger_ratelimit_LDADD = libgnuworld.la $(threadLib)

# The deprecated elog in front of the logger, on libgnuworld alone.
test_elog_shim_SOURCES = test/elog_shim.cc
test_elog_shim_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld
test_elog_shim_LDADD = libgnuworld.la $(threadLib)

# Adversarial and integration-adjacent tests written from the spec: hostile
# field values and templates through the full render/format pipeline, the
# coloured name column's exact boundaries, a null pointer of an unregistered
# type, and concurrent logging under a churning configuration.  libgnuworld
# alone, like every other test_logger_* program.
test_logger_adversarial_SOURCES = test/logger_adversarial.cc
test_logger_adversarial_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld
test_logger_adversarial_LDADD = libgnuworld.la $(threadLib)

# The two text helpers of the pushover sink: the cut that never lands inside a
# UTF-8 character and the control-character clean-up.  Both are pure text and are
# defined outside pushover.cc's HAVE_LIBCURL, so this is built and run
# everywhere - which is the point of having them outside it.
test_pushover_text_SOURCES = test/pushover_text.cc
test_pushover_text_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld \
	-I${top_srcdir}/libnotifier
test_pushover_text_LDADD = libnotifier.la libgnuworld.la $(threadLib)

# What a pushover sink of logging.conf really puts on the wire: libnotifier for
# the sink, libgnuworld for the configuration that builds it, libcurl for the
# request.  It posts to whatever PUSHOVER_TEST_URL names and says so and passes
# where that is unset, so "make check" needs no endpoint and no network.
test_pushover_delivery_SOURCES = test/pushover_delivery.cc
test_pushover_delivery_CXXFLAGS = -I${top_srcdir}/include -I${top_srcdir}/libgnuworld \
	-I${top_srcdir}/libnotifier
test_pushover_delivery_LDADD = libnotifier.la libgnuworld.la $(threadLib)

# Run by "make check".  The other programs in this directory are
# interactive or benchmarks, not tests.
TESTS = test_channelmodes test_channelbans test_checkedformat test_numbers test_events_names \
	test_logger_render \
	test_logger_format test_logger_sinks test_logger_basic test_logger_hierarchy \
	test_logger_config test_logger_ratelimit test_elog_shim test_logger_adversarial \
	test_pushover_text

if COND_LIBCURL
TESTS += test_pushover_delivery
endif
