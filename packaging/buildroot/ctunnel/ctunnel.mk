################################################################################
# ctunnel
################################################################################
CTUNNEL_VERSION = 0.1.0
CTUNNEL_SITE = https://github.com/anysoft/ctunnel.git
CTUNNEL_SITE_METHOD = git
CTUNNEL_LICENSE = BSD-2-Clause
CTUNNEL_LICENSE_FILES = LICENSE third_party/monocypher/LICENSE
CTUNNEL_DEPENDENCIES = host-python3
CTUNNEL_PROJECT_CONFIG = $(@D)/.config
CTUNNEL_CONF_OPTS = -DCTUNNEL_KCONFIG_CONFIG=$(CTUNNEL_PROJECT_CONFIG) \
	-DCTUNNEL_USE_BUNDLED_MONOCYPHER=ON -DCTUNNEL_BUILD_TESTS=OFF

define CTUNNEL_GENERATE_PROJECT_CONFIG
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) PYTHON=$(HOST_DIR)/bin/python3 \
		CONFIG=$(CTUNNEL_PROJECT_CONFIG) BUILD_DIR=$(@D)/buildroot-generated \
		$(if $(BR2_PACKAGE_CTUNNEL_MINI),buildroot_defconfig,default_defconfig)
	$(if $(BR2_PACKAGE_CTUNNEL_SERVER_ONLY),\
		$(@D)/scripts/config --file $(CTUNNEL_PROJECT_CONFIG) \
		--build-dir $(@D)/buildroot-generated --target linux --enable CTUNNEL_ROLE_SERVER_ONLY)
	$(if $(BR2_PACKAGE_CTUNNEL_IPV6_ONLY),\
		$(HOST_DIR)/bin/python3 $(@D)/scripts/kconfig/config_tool.py \
		--file $(CTUNNEL_PROJECT_CONFIG) --build-dir $(@D)/buildroot-generated \
		--target linux --disable FEATURE_IPV4)
	$(if $(BR2_PACKAGE_CTUNNEL_LINK_DYNAMIC),\
		$(HOST_DIR)/bin/python3 $(@D)/scripts/kconfig/config_tool.py \
		--file $(CTUNNEL_PROJECT_CONFIG) --build-dir $(@D)/buildroot-generated \
		--target linux --enable CTUNNEL_LINK_DYNAMIC)
	$(if $(BR2_PACKAGE_CTUNNEL_LINK_MOSTLY_STATIC),\
		$(HOST_DIR)/bin/python3 $(@D)/scripts/kconfig/config_tool.py \
		--file $(CTUNNEL_PROJECT_CONFIG) --build-dir $(@D)/buildroot-generated \
		--target linux --enable CTUNNEL_LINK_MOSTLY_STATIC)
	$(if $(BR2_PACKAGE_CTUNNEL_LINK_STATIC),\
		$(HOST_DIR)/bin/python3 $(@D)/scripts/kconfig/config_tool.py \
		--file $(CTUNNEL_PROJECT_CONFIG) --build-dir $(@D)/buildroot-generated \
		--target linux --enable CTUNNEL_LINK_STATIC)
endef
CTUNNEL_PRE_CONFIGURE_HOOKS += CTUNNEL_GENERATE_PROJECT_CONFIG
$(eval $(cmake-package))
