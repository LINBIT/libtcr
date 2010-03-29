STYLESHEET_PREFIX ?= http://docbook.sourceforge.net/release/xsl/current
MANPAGES_STYLESHEET ?= $(STYLESHEET_PREFIX)/manpages/docbook.xsl
HTML_STYLESHEET ?= $(STYLESHEET_PREFIX)/xhtml/docbook.xsl
FO_STYLESHEET ?= $(STYLESHEET_PREFIX)/fo/docbook.xsl

XSLTPROC = /usr/bin/xsltproc
XSLTPROC_OPTIONS ?= --xinclude
XSLTPROC_MANPAGES_OPTIONS ?= $(XSLTPROC_OPTIONS)
XSLTPROC_HTML_OPTIONS ?= $(XSLTPROC_OPTIONS)
XSLTPROC_FO_OPTIONS ?= $(XSLTPROC_OPTIONS)

MANPAGES       := libtc.3 tc_run.3 tc_thread_new.3 tc_thread_pool_new.3 tc_wait_fd.3 tc_mutex_lock.3 tc_signal_subscribe.3 tc_waitq_wait_event.3 tc_parallel_for.3 tc_sleep.3

.SUFFIXES: .xml .3 .html .pdf .ps

%.3: %.xml
	$(XSLTPROC) \
	$(XSLTPROC_MANPAGES_OPTIONS) \
	$(MANPAGES_STYLESHEET) $<

man:	$(MANPAGES)

clean:
	rm -f *.3
