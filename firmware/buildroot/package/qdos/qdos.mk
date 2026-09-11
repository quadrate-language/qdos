################################################################################
#
# qdos
#
################################################################################

QDOS_VERSION = local
QDOS_SITE = $(QDOS_SRC)
QDOS_SITE_METHOD = local
QDOS_LICENSE = GPL-3.0
QDOS_LICENSE_FILES = LICENSE
QDOS_DEPENDENCIES = quadrate

# Buildroot rsyncs the source tree into its build directory. Keep local build
# output out of that copy even if someone points QDOS_BUILD_DIR back inside it.
QDOS_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = --exclude build --exclude firmware/build --exclude .cache

# The simulator needs SDL3, which the device does not have and does not want.
# quadrate_src points at staging, where the quadrate package left its archives
# and headers in the dist layout.
QDOS_CONF_OPTS = \
	-Dsim=false \
	-Ddevice=true \
	-Dstatic=false \
	-Dquadrate_src=$(STAGING_DIR)/usr \
	-Dquadrate_dist=.

$(eval $(meson-package))
