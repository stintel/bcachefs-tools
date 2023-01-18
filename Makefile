PREFIX?=/usr/local
PKG_CONFIG?=pkg-config
INSTALL=install

ifeq ("$(origin V)", "command line")
  BUILD_VERBOSE = $(V)
endif
ifndef BUILD_VERBOSE
  BUILD_VERBOSE = 0
endif

ifeq ($(BUILD_VERBOSE),1)
  Q =
else
  Q = @
endif

CFLAGS+=-std=gnu11 -O2 -g -MMD -Wall -fPIC 			\
	-Wno-pointer-sign					\
	-fno-strict-aliasing					\
	-fno-delete-null-pointer-checks				\
	-I. -Iinclude -Iraid					\
	-D_FILE_OFFSET_BITS=64					\
	-D_GNU_SOURCE						\
	-D_LGPL_SOURCE						\
	-DRCU_MEMBARRIER					\
	-DZSTD_STATIC_LINKING_ONLY				\
	-DFUSE_USE_VERSION=32					\
	-DNO_BCACHEFS_CHARDEV					\
	-DNO_BCACHEFS_FS					\
	-DNO_BCACHEFS_SYSFS					\
	-DVERSION_STRING='"$(VERSION)"'				\
	$(EXTRA_CFLAGS)
LDFLAGS+=$(CFLAGS) $(EXTRA_LDFLAGS)

## Configure Tools
PYTEST_ARGS?=
PYTEST_CMD?=$(shell \
	command -v pytest-3 \
	|| which pytest-3 2>/dev/null \
)
PYTEST:=$(PYTEST_CMD) $(PYTEST_ARGS)

CARGO_ARGS=
CARGO=cargo $(CARGO_ARGS)
CARGO_PROFILE=release
# CARGO_PROFILE=debug

CARGO_BUILD_ARGS=--$(CARGO_PROFILE)
CARGO_BUILD=$(CARGO) build $(CARGO_BUILD_ARGS)
VERSION?=$(shell git describe --dirty=+ 2>/dev/null || echo v0.1-nogit)

include Makefile.compiler

CFLAGS+=$(call cc-disable-warning, unused-but-set-variable)
CFLAGS+=$(call cc-disable-warning, stringop-overflow)
CFLAGS+=$(call cc-disable-warning, zero-length-bounds)
CFLAGS+=$(call cc-disable-warning, missing-braces)
CFLAGS+=$(call cc-disable-warning, zero-length-array)
CFLAGS+=$(call cc-disable-warning, shift-overflow)
CFLAGS+=$(call cc-disable-warning, enum-conversion)

PKGCONFIG_LIBS="blkid uuid liburcu libsodium zlib liblz4 libzstd libudev libkeyutils"
ifdef BCACHEFS_FUSE
	PKGCONFIG_LIBS+="fuse3 >= 3.7"
	CFLAGS+=-DBCACHEFS_FUSE
endif

PKGCONFIG_CFLAGS:=$(shell $(PKG_CONFIG) --cflags $(PKGCONFIG_LIBS))
ifeq (,$(PKGCONFIG_CFLAGS))
    $(error pkg-config error, command: $(PKG_CONFIG) --cflags $(PKGCONFIG_LIBS))
endif
PKGCONFIG_LDLIBS:=$(shell $(PKG_CONFIG) --libs   $(PKGCONFIG_LIBS))
ifeq (,$(PKGCONFIG_LDLIBS))
    $(error pkg-config error, command: $(PKG_CONFIG) --libs $(PKGCONFIG_LIBS))
endif

CFLAGS+=$(PKGCONFIG_CFLAGS)
LDLIBS+=$(PKGCONFIG_LDLIBS)
LDLIBS+=-lm -lpthread -lrt -lkeyutils -laio -ldl
LDLIBS+=-L. -lbcachefs_rs
LDLIBS+=$(EXTRA_LDLIBS)

ifeq ($(PREFIX),/usr)
	ROOT_SBINDIR=/sbin
	INITRAMFS_DIR=$(PREFIX)/share/initramfs-tools
else
	ROOT_SBINDIR=$(PREFIX)/sbin
	INITRAMFS_DIR=/etc/initramfs-tools
endif

.PHONY: all
all: bcachefs_rs bcachefs lib

.PHONY: lib
lib: libbcachefs.so

.PHONY: tests
tests: tests/test_helper

.PHONY: check
check: tests bcachefs
ifneq (,$(PYTEST_CMD))
	$(PYTEST)
else
	@echo "WARNING: pytest not found or specified, tests could not be run."
endif

.PHONY: TAGS tags
TAGS:
	ctags -e -R .

tags:
	ctags -R .

SRCS=$(shell find . -type f ! -path '*/.*/*' -iname '*.c')
DEPS=$(SRCS:.c=.d)
-include $(DEPS)

OBJS=$(SRCS:.c=.o)

%.o: %.c
	@echo "    [CC]     $@"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

bcachefs: $(filter-out ./tests/%.o, $(OBJS))
	@echo "    [LD]     $@"
	$(Q)$(CC) $(LDFLAGS) $+ $(LOADLIBES) $(LDLIBS) -o $@

RUST_SRCS=$(shell find rust-src/ -type f -iname '*.rs')
BCACHEFS_RS_SRCS=$(filter %bcachefs_rs, $(RUST_SRCS))
MOUNT_SRCS=$(filter %mount, $(RUST_SRCS))

debug: CFLAGS+=-Werror -DCONFIG_BCACHEFS_DEBUG=y -DCONFIG_VALGRIND=y
debug: bcachefs

MOUNT_OBJ=$(filter-out ./bcachefs.o ./tests/%.o ./cmd_%.o , $(OBJS))
libbcachefs.so: LDFLAGS+=-shared
libbcachefs.so: $(MOUNT_OBJ)
	@echo "    [CC]     $@"
	$(Q)$(CC) $(LDFLAGS) $+ -o $@ $(LDLIBS)

BCACHEFS_RS_TOML=rust-src/bcachefs_rs/Cargo.toml
bcachefs_rs: $(BCACHEFS_RS_SRCS)
	$(CARGO_BUILD) --manifest-path $(BCACHEFS_RS_TOML)

	ln -f rust-src/bcachefs_rs/target/$(CARGO_PROFILE)/libbcachefs_rs.so

MOUNT_TOML=rust-src/mount/Cargo.toml
mount.bcachefs: lib $(MOUNT_SRCS)
	LIBBCACHEFS_LIB=$(CURDIR) \
	LIBBCACHEFS_INCLUDE=$(CURDIR) \
	$(CARGO_BUILD) --manifest-path $(MOUNT_TOML)

	ln -f rust-src/mount/target/$(CARGO_PROFILE)/bcachefs-mount $@

tests/test_helper: $(filter ./tests/%.o, $(OBJS))
	@echo "    [LD]     $@"
	$(Q)$(CC) $(LDFLAGS) $+ $(LOADLIBES) $(LDLIBS) -o $@

# If the version string differs from the last build, update the last version
ifneq ($(VERSION),$(shell cat .version 2>/dev/null))
.PHONY: .version
endif
.version:
	@echo "  [VERS]    $@"
	$(Q)echo '$(VERSION)' > $@

# Rebuild the 'version' command any time the version string changes
cmd_version.o : .version

.PHONY: install
install: INITRAMFS_HOOK=$(INITRAMFS_DIR)/hooks/bcachefs
install: INITRAMFS_SCRIPT=$(INITRAMFS_DIR)/scripts/local-premount/bcachefs
install: bcachefs lib
	$(INSTALL) -m0755 -D bcachefs      -t $(DESTDIR)$(ROOT_SBINDIR)
	$(INSTALL) -m0755    fsck.bcachefs    $(DESTDIR)$(ROOT_SBINDIR)
	$(INSTALL) -m0755    mkfs.bcachefs    $(DESTDIR)$(ROOT_SBINDIR)
	$(INSTALL) -m0644 -D bcachefs.8    -t $(DESTDIR)$(PREFIX)/share/man/man8/
	$(INSTALL) -m0755 -D initramfs/script $(DESTDIR)$(INITRAMFS_SCRIPT)
	$(INSTALL) -m0755 -D initramfs/hook   $(DESTDIR)$(INITRAMFS_HOOK)
	$(INSTALL) -m0755 -D mount.bcachefs.sh $(DESTDIR)$(ROOT_SBINDIR)
	$(INSTALL) -m0755 -D libbcachefs.so -t $(DESTDIR)$(PREFIX)/lib/
	$(INSTALL) -m0755 -D libbcachefs_rs.so -t $(DESTDIR)$(PREFIX)/lib/

	sed -i '/^# Note: make install replaces/,$$d' $(DESTDIR)$(INITRAMFS_HOOK)
	echo "copy_exec $(ROOT_SBINDIR)/bcachefs /sbin/bcachefs" >> $(DESTDIR)$(INITRAMFS_HOOK)

.PHONY: clean
clean:
	@echo "Cleaning all"
	$(Q)$(RM) bcachefs mount.bcachefs libbcachefs.so libbcachefs_mount.a libbcachefs_rs.so tests/test_helper .version *.tar.xz $(OBJS) $(DEPS) $(DOCGENERATED)
	$(Q)$(RM) -rf rust-src/*/target

.PHONY: deb
deb: all
	debuild -us -uc -nc -b -i -I

bcachefs-principles-of-operation.pdf: doc/bcachefs-principles-of-operation.tex
	pdflatex doc/bcachefs-principles-of-operation.tex
	pdflatex doc/bcachefs-principles-of-operation.tex

doc: bcachefs-principles-of-operation.pdf

.PHONY: update-bcachefs-sources
update-bcachefs-sources:
	git rm -rf --ignore-unmatch libbcachefs
	test -d libbcachefs || mkdir libbcachefs
	cp $(LINUX_DIR)/fs/bcachefs/*.[ch] libbcachefs/
	git add libbcachefs/*.[ch]
	cp $(LINUX_DIR)/include/trace/events/bcachefs.h include/trace/events/
	git add include/trace/events/bcachefs.h
	cp $(LINUX_DIR)/include/linux/xxhash.h include/linux/
	git add include/linux/xxhash.h
	cp $(LINUX_DIR)/lib/xxhash.c linux/
	git add linux/xxhash.c
	cp $(LINUX_DIR)/kernel/locking/six.c linux/
	git add linux/six.c
	cp $(LINUX_DIR)/include/linux/six.h include/linux/
	git add include/linux/six.h
	cp $(LINUX_DIR)/include/linux/list_nulls.h include/linux/
	git add include/linux/list_nulls.h
	cp $(LINUX_DIR)/include/linux/poison.h include/linux/
	git add include/linux/poison.h
	cp $(LINUX_DIR)/include/linux/generic-radix-tree.h include/linux/
	git add include/linux/generic-radix-tree.h
	cp $(LINUX_DIR)/lib/generic-radix-tree.c linux/
	git add linux/generic-radix-tree.c
	cp $(LINUX_DIR)/include/linux/kmemleak.h include/linux/
	git add include/linux/kmemleak.h
	cp $(LINUX_DIR)/include/linux/printbuf.h include/linux/
	git add include/linux/printbuf.h
	cp $(LINUX_DIR)/lib/printbuf.c linux/
	git add linux/printbuf.c
	cp $(LINUX_DIR)/lib/math/mean_and_variance.c linux/
	git add linux/mean_and_variance.c
	cp $(LINUX_DIR)/include/linux/mean_and_variance.h include/linux/
	git add include/linux/mean_and_variance.h
	cp $(LINUX_DIR)/lib/math/int_sqrt.c linux/
	git add linux/int_sqrt.c
	cp $(LINUX_DIR)/scripts/Makefile.compiler ./
	git add Makefile.compiler
	$(RM) libbcachefs/*.mod.c
	git -C $(LINUX_DIR) rev-parse HEAD | tee .bcachefs_revision
	git add .bcachefs_revision


.PHONY: update-commit-bcachefs-sources
update-commit-bcachefs-sources: update-bcachefs-sources
	git commit -m "Update bcachefs sources to $(shell git -C $(LINUX_DIR) show --oneline --no-patch)"

SRCTARXZ = bcachefs-tools-$(VERSION).tar.xz
SRCDIR=bcachefs-tools-$(VERSION)

.PHONY: tarball
tarball: $(SRCTARXZ)

$(SRCTARXZ) : .gitcensus
	$(Q)tar --transform "s,^,$(SRCDIR)/," -Jcf $(SRCDIR).tar.xz  \
	    `cat .gitcensus` 
	@echo Wrote: $@

.PHONY: .gitcensus
.gitcensus:
	$(Q)if test -d .git; then \
	  git ls-files > .gitcensus; \
	fi
