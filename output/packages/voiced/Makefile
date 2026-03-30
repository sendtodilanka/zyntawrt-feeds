# SPDX-License-Identifier: GPL-2.0-or-later
#
# OpenWrt package Makefile for voiced — VoLTE call control daemon

include $(TOPDIR)/rules.mk

PKG_NAME:=voiced
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

PKG_LICENSE:=GPL-2.0-or-later
PKG_MAINTAINER:=ZyntaWRT Project

include $(INCLUDE_DIR)/package.mk

define Package/voiced
  SECTION:=telephony
  CATEGORY:=Telephony
  TITLE:=VoLTE/IMS call control daemon for AW1000
  DEPENDS:=+kmod-slic-si3226x +kmod-ipq-pcm-tdm
  PKGARCH:=all
endef

define Package/voiced/description
  Event-driven daemon that bridges the Quectel modem AT command interface
  with the Si3226x SLIC driver to provide landline-style telephone service
  over VoLTE/IMS on the Arcadyan AW1000 router.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/voiced.c $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=c11 -Wall -Wextra -O2 \
		-o $(PKG_BUILD_DIR)/voiced \
		$(PKG_BUILD_DIR)/voiced.c
endef

define Package/voiced/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/voiced $(1)/usr/sbin/voiced

	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/voiced.init $(1)/etc/init.d/voiced
endef

$(eval $(call BuildPackage,voiced))
