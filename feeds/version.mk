# Unified version management for qmodem packages
# Sourced from FUjr/QModem — included via "../../version.mk" by all luci-app-qmodem* packages.
# Placed here because packages were imported from QModem/luci/ into feeds/qmodem/,
# shifting the relative path by one level.
define qmodem_commitcount
$(shell \
  if git log -1 >/dev/null 2>/dev/null; then \
    if [ -n "$(1)" ]; then \
      last_bump="$$(git log --pretty=format:'%h %s' . | \
        grep -m 1 -e ': [uU]pdate to ' -e ': [bB]ump to ' | \
        cut -f 1 -d ' ')"; \
    fi; \
    if [ -n "$$last_bump" ]; then \
      echo -n $$(($$(git rev-list --count "$$last_bump..HEAD" .) + 1)); \
    else \
      echo -n $$(git rev-list --count HEAD .); \
    fi; \
  else \
    echo -n 0; \
  fi)
endef

QMODEM_COMMITCOUNT = $(if $(DUMP),0,$(call qmodem_commitcount))
QMODEM_AUTORELEASE = $(if $(DUMP),0,$(call qmodem_commitcount,1))

QMODEM_VERSION:=3.0.2
QMODEM_RELEASE:=$(QMODEM_AUTORELEASE)

