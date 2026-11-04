################################################################################
#
# quadrate
#
# The release QDOS builds against; cmake/quadrate.cmake and
# subprojects/quadrate.wrap pin the same one for the other builds.
#
# Only the libraries QDOS needs are built: the runtime, the compiler front-end
# and the AST interpreter, plus the two standard library modules QDOS links.
# build_tools=false and build_compiler=false drop the command-line tools and
# with them LLVM; stdlib_modules=math,decimal drops every other module and with
# them OpenSSL. Neither is wanted in a calculator.
#
# The two libraries Quadrate's Meson build takes as subprojects, u8t and
# libmpdec, come in as extra downloads. Buildroot runs Meson with downloads
# off, so they are unpacked where its wraps would put them, with the build
# files from subprojects/packagefiles laid over them as Meson would.
#
# build-image.sh can point the package at a local tree instead (QUADRATE_DIR),
# through QUADRATE_OVERRIDE_SRCDIR. That tree brings its own subprojects.
#
# The archives are staged by hand rather than by `meson install`, because
# Quadrate's build does not install its static libraries -- its own release
# process repacks them with ar, and several are thin archives that only
# reference objects back in the build tree.
#
################################################################################

QUADRATE_VERSION = 0.7.0
QUADRATE_SITE = $(call github,quadrate-language,quadrate,$(QUADRATE_VERSION))
QUADRATE_LICENSE = GPL-3.0, Apache-2.0
QUADRATE_LICENSE_FILES = LICENSE LICENSE-STDLIB
QUADRATE_INSTALL_STAGING = YES
QUADRATE_INSTALL_TARGET = NO
QUADRATE_DEPENDENCIES = host-pkgconf

QUADRATE_U8T_VERSION = 1.4.2
QUADRATE_MPDEC_VERSION = 4.0.1
QUADRATE_EXTRA_DOWNLOADS = \
	https://github.com/klahr/libu8t/archive/$(QUADRATE_U8T_VERSION)/libu8t-$(QUADRATE_U8T_VERSION).tar.gz \
	https://www.bytereef.org/software/mpdecimal/releases/mpdecimal-$(QUADRATE_MPDEC_VERSION).tar.gz

define QUADRATE_EXTRACT_SUBPROJECTS
	mkdir -p $(@D)/subprojects/u8t $(@D)/subprojects/mpdecimal-$(QUADRATE_MPDEC_VERSION)
	$(call suitable-extractor,libu8t-$(QUADRATE_U8T_VERSION).tar.gz) \
		$(QUADRATE_DL_DIR)/libu8t-$(QUADRATE_U8T_VERSION).tar.gz | \
		$(TAR) --strip-components=1 -C $(@D)/subprojects/u8t $(TAR_OPTIONS) -
	$(call suitable-extractor,mpdecimal-$(QUADRATE_MPDEC_VERSION).tar.gz) \
		$(QUADRATE_DL_DIR)/mpdecimal-$(QUADRATE_MPDEC_VERSION).tar.gz | \
		$(TAR) --strip-components=1 -C $(@D)/subprojects/mpdecimal-$(QUADRATE_MPDEC_VERSION) $(TAR_OPTIONS) -
	cp -a $(@D)/subprojects/packagefiles/u8t/. $(@D)/subprojects/u8t/
	cp -a $(@D)/subprojects/packagefiles/mpdecimal/. $(@D)/subprojects/mpdecimal-$(QUADRATE_MPDEC_VERSION)/
endef
QUADRATE_POST_EXTRACT_HOOKS += QUADRATE_EXTRACT_SUBPROJECTS

QUADRATE_CONF_OPTS = \
	-Dbuild_tools=false \
	-Dbuild_compiler=false \
	-Dstdlib_modules=math,decimal \
	-Dbuild_tests=false \
	-Dbuild_examples=false \
	-Dwerror=false

# The libraries QDOS links, in dependency order
QUADRATE_LIBS = interp qc math decimal rt u8t

define QUADRATE_BUILD_CMDS
	$(NINJA) $(NINJA_OPTS) -C $(@D)/buildroot-build \
		lib/interp/libinterp.a lib/qc/libqc.a lib/rt/librt_static.a \
		stdlib/math/libmath.a stdlib/decimal/libdecimal.a subprojects/u8t/libu8t.a
endef

# Staged in the layout QDOS's build expects: dist/lib/quadrate and dist/include,
# reachable as -Dquadrate_src=$(STAGING_DIR)/usr -Dquadrate_dist=.
define QUADRATE_INSTALL_STAGING_CMDS
	mkdir -p $(STAGING_DIR)/usr/lib/quadrate $(STAGING_DIR)/usr/include/quadrate
	cd $(@D)/buildroot-build/lib/interp && $(TARGET_AR) rcs libinterp_packed.a $$($(TARGET_AR) -t libinterp.a)
	cd $(@D)/buildroot-build/lib/qc && $(TARGET_AR) rcs libqc_packed.a $$($(TARGET_AR) -t libqc.a)
	cd $(@D)/buildroot-build/lib/rt && $(TARGET_AR) rcs librt_packed.a $$($(TARGET_AR) -t librt_static.a)
	cd $(@D)/buildroot-build/stdlib/math && $(TARGET_AR) rcs libmath_packed.a $$($(TARGET_AR) -t libmath.a)
	cd $(@D)/buildroot-build/stdlib/decimal && $(TARGET_AR) rcs libdecimal_packed.a $$($(TARGET_AR) -t libdecimal.a)
	cd $(@D)/buildroot-build/subprojects/u8t && $(TARGET_AR) rcs libu8t_packed.a $$($(TARGET_AR) -t libu8t.a)
	$(INSTALL) -m 0644 $(@D)/buildroot-build/lib/interp/libinterp_packed.a $(STAGING_DIR)/usr/lib/quadrate/libinterp.a
	$(INSTALL) -m 0644 $(@D)/buildroot-build/lib/qc/libqc_packed.a $(STAGING_DIR)/usr/lib/quadrate/libqc.a
	$(INSTALL) -m 0644 $(@D)/buildroot-build/lib/rt/librt_packed.a $(STAGING_DIR)/usr/lib/quadrate/librt.a
	$(INSTALL) -m 0644 $(@D)/buildroot-build/stdlib/math/libmath_packed.a $(STAGING_DIR)/usr/lib/quadrate/libmath.a
	$(INSTALL) -m 0644 $(@D)/buildroot-build/stdlib/decimal/libdecimal_packed.a $(STAGING_DIR)/usr/lib/quadrate/libdecimal.a
	$(INSTALL) -m 0644 $(@D)/buildroot-build/subprojects/u8t/libu8t_packed.a $(STAGING_DIR)/usr/lib/quadrate/libu8t.a
	cp -a $(@D)/lib/interp/include/quadrate/interp $(STAGING_DIR)/usr/include/quadrate/
	cp -a $(@D)/lib/qc/include/quadrate/qc $(STAGING_DIR)/usr/include/quadrate/
	cp -a $(@D)/lib/rt/include/quadrate/rt $(STAGING_DIR)/usr/include/quadrate/
	cp -a $(@D)/stdlib/math/include/quadrate/math $(STAGING_DIR)/usr/include/quadrate/
	cp -a $(@D)/stdlib/decimal/include/quadrate/decimal $(STAGING_DIR)/usr/include/quadrate/
endef

$(eval $(meson-package))
