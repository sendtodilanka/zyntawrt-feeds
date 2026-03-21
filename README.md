# zyntawrt-feeds

Custom OpenWrt package feed for [ZyntaWRT](https://github.com/sendtodilanka/ZyntaWRT) — daily synced third-party packages for the Arcadyan AW1000.

## Packages

| Package | Source |
|---|---|
| `luci-app-openclash` | vernesong/OpenClash |
| `luci-app-passwall` | xiaorouji/openwrt-passwall |
| `luci-app-passwall2` | xiaorouji/openwrt-passwall2 |
| `luci-app-qmodem` + modules | FUjr/modem_feeds |
| `luci-app-3ginfo-lite` | 4IceG/luci-app-3ginfo-lite |
| `luci-theme-argon` | jerrykuku/luci-theme-argon |
| `luci-app-argon-config` | jerrykuku/luci-app-argon-config |
| `luci-app-homeproxy` | immortalwrt/homeproxy |
| `luci-app-adguardhome` | rufengsuixing/luci-app-adguardhome |

## Usage (in OpenWrt Buildroot)

Add to `feeds.conf`:

```
src-git zyntawrt https://github.com/sendtodilanka/zyntawrt-feeds.git;main
```

Then:

```bash
./scripts/feeds update zyntawrt
./scripts/feeds install -a -p zyntawrt
```

## Auto-sync

Packages are automatically synced from upstream daily at 2 AM UTC via GitHub Actions.
Manual sync: trigger `workflow_dispatch` on the Actions tab.

## License

Individual packages retain their original licenses. Feed infrastructure: MIT.
