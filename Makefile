TARGETS = jkimlog jkrdlog jktask
BINDIR = /usr/local/bin

# Override the following directory locations if needed.
#NICOJK_LOG_DIR = /var/local/nicojk
#JKTASK_BASE_DIR = /var/local/jktask
#EDCB_INI_ROOT = /var/local/edcb
#JKCNSL_UNIX_BASE_DIR = /var/local/jkcnsl

ifdef NICOJK_LOG_DIR
  export CPPFLAGS += -DNICOJK_LOG_DIR=\"$(NICOJK_LOG_DIR)\"
endif
ifdef JKTASK_BASE_DIR
  export CPPFLAGS += -DJKTASK_BASE_DIR=\"$(JKTASK_BASE_DIR)\"
endif
ifdef EDCB_INI_ROOT
  export CPPFLAGS += -DEDCB_INI_ROOT=\"$(EDCB_INI_ROOT)\"
endif
ifdef JKCNSL_UNIX_BASE_DIR
  export CPPFLAGS += -DJKCNSL_UNIX_BASE_DIR=\"$(JKCNSL_UNIX_BASE_DIR)\"
endif

all: $(addsuffix .all, $(TARGETS))
clean: $(addsuffix .clean, $(TARGETS))
install: $(addsuffix .install, $(TARGETS))
%.all:
	$(MAKE) -C $*
%.clean:
	$(MAKE) -C $* clean
%.install: %.all
	install $*/$* $(BINDIR)
setup:
	$(if $(setup_user),,$(error Specify setup_user))
	$(if $(setup_group),,$(error Specify setup_group))
	[ -d $(or $(NICOJK_LOG_DIR),/var/local/nicojk) ] || install -o $(setup_user) -g $(setup_group) -m 0750 -d $(or $(NICOJK_LOG_DIR),/var/local/nicojk)
	[ -d $(or $(JKTASK_BASE_DIR),/var/local/jktask) ] || install -o $(setup_user) -g $(setup_group) -m 0750 -d $(or $(JKTASK_BASE_DIR),/var/local/jktask)
	[ -f $(or $(JKTASK_BASE_DIR),/var/local/jktask)/jktask.ini ] || install -o $(setup_user) -g $(setup_group) -m 0644 jktask/jktask.ini $(or $(JKTASK_BASE_DIR),/var/local/jktask)/jktask.ini
	[ -f /etc/systemd/system/jktask.service ] || sed -e s#{BINDIR}#$(BINDIR)#g -e s/{USER}/$(setup_user)/g jktask/jktask.service.txt >/etc/systemd/system/jktask.service
