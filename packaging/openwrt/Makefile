include $(TOPDIR)/rules.mk
PKG_NAME:=ctunnel
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/anysoft/ctunnel.git
PKG_SOURCE_VERSION:=v$(PKG_VERSION)
PKG_LICENSE:=BSD-2-Clause
PKG_LICENSE_FILES:=LICENSE third_party/monocypher/LICENSE
PKG_BUILD_DEPENDS:=python3/host
include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk
define Package/ctunnel
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Authenticated reverse TCP tunnel
endef

define Package/ctunnel/Default
  SECTION:=net
  CATEGORY:=Network
endef

define Package/ctunnel-mini-server
  $(call Package/ctunnel/Default)
  TITLE:=ctunnel minimal server
  VARIANT:=mini-server
endef

define Package/ctunnel-client
  $(call Package/ctunnel/Default)
  TITLE:=ctunnel minimal client
  VARIANT:=client
endef

define Package/ctunnel-full
  $(call Package/ctunnel/Default)
  TITLE:=ctunnel full feature set
  VARIANT:=full
endef

ifeq ($(BUILD_VARIANT),client)
  CTUNNEL_DEFCONFIG:=configs/mini_client_defconfig
else ifeq ($(BUILD_VARIANT),full)
  CTUNNEL_DEFCONFIG:=configs/full_defconfig
else
  CTUNNEL_DEFCONFIG:=configs/openwrt_mini_defconfig
endif

CMAKE_OPTIONS += -DCTUNNEL_KCONFIG_CONFIG=$(PKG_BUILD_DIR)/.config \
	-DCTUNNEL_USE_BUNDLED_MONOCYPHER=ON -DCTUNNEL_BUILD_TESTS=OFF
CTUNNEL_LINK_SYMBOL?=CTUNNEL_LINK_MOSTLY_STATIC

define Build/Configure
	python3 $(PKG_BUILD_DIR)/scripts/kconfig/configure.py \
		--config $(PKG_BUILD_DIR)/.config --build-dir $(PKG_BUILD_DIR)/kconfig-generated \
		--target linux defconfig $(PKG_BUILD_DIR)/$(CTUNNEL_DEFCONFIG)
	python3 $(PKG_BUILD_DIR)/scripts/kconfig/config_tool.py \
		--file $(PKG_BUILD_DIR)/.config --build-dir $(PKG_BUILD_DIR)/kconfig-generated \
		--target linux --enable $(CTUNNEL_LINK_SYMBOL)
	$(call Build/Configure/Default)
endef
define Package/ctunnel/install
	$(INSTALL_DIR) $(1)/usr/bin $(1)/etc/init.d $(1)/etc/ctunnel
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/ctunnel $(1)/usr/bin/
	$(INSTALL_BIN) ./files/ctunnel.init $(1)/etc/init.d/ctunnel
endef
define Package/ctunnel-mini-server/install
	$(call Package/ctunnel/install,$(1))
endef
define Package/ctunnel-client/install
	$(call Package/ctunnel/install,$(1))
endef
define Package/ctunnel-full/install
	$(call Package/ctunnel/install,$(1))
endef
$(eval $(call BuildPackage,ctunnel))
$(eval $(call BuildPackage,ctunnel-mini-server))
$(eval $(call BuildPackage,ctunnel-client))
$(eval $(call BuildPackage,ctunnel-full))
