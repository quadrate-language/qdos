################################################################################
#
# quadrate
#
# Only the libraries QDOS needs are built: the runtime, the compiler front-end
# and the AST interpreter. build_tools=false drops the command-line tools and
# with them LLVM; build_stdlib=false drops the standard library modules and with
# them OpenSSL and zlib. Neither is wanted in a calculator.
#
# The archives are staged by hand rather than by `meson install`, because
# Quadrate's build does not install its static libraries -- its own release
# process repacks them with ar, and several are thin archives that only
# reference objects back in the build tree.
#
################################################################################

QUADRATE_VERSION = local
QUADRATE_SITE = $(QDOS_QUADRATE_SRC)
QUADRATE_SITE_METHOD = local
QUADRATE_LICENSE = GPL-3.0, Apache-2.0
QUADRATE_LICENSE_FILES = LICENSE LICENSE-STDLIB
QUADRATE_INSTALL_STAGING = YES
QUADRATE_INSTALL_TARGET = NO
QUADRATE_DEPENDENCIES = host-pkgconf

QUADRATE_CONF_OPTS = \
	-Dbuild_tools=false \
	-Dbuild_stdlib=false \
	-Dbuild_tests=false \
	-Dbuild_examples=false \
	-Dwerror=false

# The libraries QDOS links, in dependency order
QUADRATE_LIBS = interp qc rt u8t

define QUADRATE_BUILD_CMDS
	$(NINJA) $(NINJA_OPTS) -C $(@D)/buildroot-build \
		lib/interp/libinterp.a lib/qc/libqc.a lib/rt/librt_static.a subprojects/u8t/libu8t.a
endef

# Staged in the layout QDOS's build expects: dist/lib/quadrate and dist/include,
# reachable as -Dquadrate_src=$(STAGING_DIR)/usr -Dquadrate_dist=.
define QUADRATE_INSTALL_STAGING_CMDS
	mkdir -p $(STAGING_DIR)/usr/lib/quadrate $(STAGING_DIR)/usr/include/quadrate
	cd $(@D)/buildroot-build/lib/interp && $(TARGET_AR) rcs libinterp_packed.a $$($(TARGET_AR) -t libinterp.a)
	cd $(@D)/buildroot-build/lib/qc && $(TARGET_AR) rcs libqc_packed.a $$($(TARGET_AR) -t libqc.a)
	cd $(@D)/buildroot-build/lib/rt && $(TARGET_AR) rcs librt_packed.a $$($(TARGET_AR) -t librt_static.a)
	cd $(@D)/buildroot-build/subprojects/u8t && $(TARGET_AR) rcs libu8t_packed.a $$($(TARGET_AR) -t libu8t.a)
	$(INSTALL) -m 0644 $(@D)/buildroot-build/lib/interp/libinterp_packed.a $(STAGING_DIR)/usr/lib/quadrate/libinterp.a
	$(INSTALL) -m 0644 $(@D)/buildroot-build/lib/qc/libqc_packed.a $(STAGING_DIR)/usr/lib/quadrate/libqc.a
	$(INSTALL) -m 0644 $(@D)/buildroot-build/lib/rt/librt_packed.a $(STAGING_DIR)/usr/lib/quadrate/librt.a
	$(INSTALL) -m 0644 $(@D)/buildroot-build/subprojects/u8t/libu8t_packed.a $(STAGING_DIR)/usr/lib/quadrate/libu8t.a
	cp -a $(@D)/lib/interp/include/quadrate/interp $(STAGING_DIR)/usr/include/quadrate/
	cp -a $(@D)/lib/qc/include/quadrate/qc $(STAGING_DIR)/usr/include/quadrate/
	cp -a $(@D)/lib/rt/include/quadrate/rt $(STAGING_DIR)/usr/include/quadrate/
endef

$(eval $(meson-package))
