plugins_LTLIBRARIES += \
	i3bar.la

i3bar_la_SOURCES = \
	src/output/i3bar/i3bar.c

i3bar_la_CFLAGS = \
	$(AM_CFLAGS) \
	$(YAJL_CFLAGS) \
	$(GLIB_CFLAGS)

i3bar_la_LDFLAGS = \
	$(AM_LDFLAGS) \
	-module -avoid-version -export-symbols-regex j4status_output

i3bar_la_LIBADD = \
	libj4status.la \
	$(YAJL_LIBS) \
	$(GLIB_LIBS)

XSLTPROC_CONDITIONS += enable_i3bar_output

man5_MANS += \
	man/j4status-i3bar.conf.5
