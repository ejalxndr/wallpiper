# [2.1.0](https://github.com/ejalxndr/wallpiper/compare/v2.0.1...v2.1.0) (2026-09-13)


### Bug Fixes

* **kde:** retry unbound capture channels instead of dropping them forever ([0ec562d](https://github.com/ejalxndr/wallpiper/commit/0ec562dbcb5baa0d6ed1364bf4f72bc619d6a8a2))
* **portals:** resolve real VkFormat instead of hardcoding DRM XRGB8888 for dmabuf import ([02bbaf4](https://github.com/ejalxndr/wallpiper/commit/02bbaf4410c98c4a46ea53a49218bb707778f642))
* **wl-common:** retry unbound capture channels every loop tick, not just once ([14fd4f1](https://github.com/ejalxndr/wallpiper/commit/14fd4f1969f56de015eeecd0d0d2dd27c08ad857))


### Features

* add support for PROTON_ENABLE_WAYLAND=1 ([38c4c6c](https://github.com/ejalxndr/wallpiper/commit/38c4c6cdec0da52c211c6be8b514024725b39c30))
* Add wallpaper capture API: `wallpiperctl capture <monitor; int; 0-indexed> <path>` ([a2dc9b4](https://github.com/ejalxndr/wallpiper/commit/a2dc9b42eb3ce4326aab2745ad91c4b06b830046))

## [2.0.1](https://github.com/shdwmtr/wallpiper/compare/v2.0.0...v2.0.1) (2026-08-29)


### Bug Fixes

* Fix GNOME portal not working on rotated monitors ([6fe8a9c](https://github.com/shdwmtr/wallpiper/commit/6fe8a9c74ebc5a9e11206c4cf014318d395faccc))
* Fix monitor positioning algorithm ([d81322e](https://github.com/shdwmtr/wallpiper/commit/d81322eab319174db4caf3231cd7aedc59e62613))

# [2.0.0](https://github.com/shdwmtr/wallpiper/compare/v1.4.0...v2.0.0) (2026-08-26)


### Bug Fixes

* **ci:** fix semantic-release extends resolution for local config ([399f098](https://github.com/shdwmtr/wallpiper/commit/399f0987958f4c7b204ab51617f7dcf4c054cb64))


### Code Refactoring

* refactor code-base to prioritize simplicity, matching existing projects like wine/proton. ([6ba7923](https://github.com/shdwmtr/wallpiper/commit/6ba792341fe3669902d071ecfe63de851b15e72d))


### BREAKING CHANGES

* build directory moved from ./target to ./build

# [1.4.0](https://github.com/shdwmtr/wallpiper/compare/v1.3.0...v1.4.0) (2026-08-26)


### Bug Fixes

* serialize protocol/wl-common sub-builds to fix parallel make race. Closes [#17](https://github.com/shdwmtr/wallpiper/issues/17) ([fea01d9](https://github.com/shdwmtr/wallpiper/commit/fea01d9b74ff102fb55bfe08d8b4e5ebcb054e1a))


### Features

* add --help/-h to wallpiperd ([efa6023](https://github.com/shdwmtr/wallpiper/commit/efa6023c8435fd19bbadd523f15f205063cc8c13))

# [1.3.0](https://github.com/shdwmtr/wallpiper/compare/v1.2.2...v1.3.0) (2026-08-25)


### Features

* add tray translation options `native`, `notray`, `passthrough` ([6bb89f3](https://github.com/shdwmtr/wallpiper/commit/6bb89f3c01b005a0ecd53c73b651cdab7daad6c3))

## [1.2.2](https://github.com/shdwmtr/wallpiper/compare/v1.2.1...v1.2.2) (2026-08-25)


### Bug Fixes

* Fix tray rendering issues, and tray being duplicated on GNOME and KDE. Closes [#8](https://github.com/shdwmtr/wallpiper/issues/8) ([75ac9fb](https://github.com/shdwmtr/wallpiper/commit/75ac9fb79d33d42c05c620cc90628872219a7d7f))

## [1.2.1](https://github.com/shdwmtr/wallpiper/compare/v1.2.0...v1.2.1) (2026-08-25)


### Bug Fixes

* fix wallpiper failing to IPC with external portals (GNOME, KDE) ([d690ead](https://github.com/shdwmtr/wallpiper/commit/d690ead69de98203a94cdd1066ced9545ac61c75))

# [1.2.0](https://github.com/shdwmtr/wallpiper/compare/v1.1.4...v1.2.0) (2026-08-25)


### Features

* Disable hardware acceleration by default (still able to turn back on in WE Settings) ([8768ef0](https://github.com/shdwmtr/wallpiper/commit/8768ef032f87494d14572321fbce247f707b53ba))

## [1.1.4](https://github.com/shdwmtr/wallpiper/compare/v1.1.3...v1.1.4) (2026-08-25)


### Bug Fixes

* fix fsutil overwriting config ([a6cba39](https://github.com/shdwmtr/wallpiper/commit/a6cba3942fe270af412b00b26ad335f86385ff13))

## [1.1.3](https://github.com/shdwmtr/wallpiper/compare/v1.1.2...v1.1.3) (2026-08-25)


### Bug Fixes

* fix wallpaper engines broken default installation. Closes [#14](https://github.com/shdwmtr/wallpiper/issues/14) ([c00bf2d](https://github.com/shdwmtr/wallpiper/commit/c00bf2db3e4ab20b69c1205e7e8c6861c8cdeefa))

## [1.1.2](https://github.com/shdwmtr/wallpiper/compare/v1.1.1...v1.1.2) (2026-08-25)


### Bug Fixes

* stop targeting arbitrary python3 processes as old proton wrappers. Closes [#13](https://github.com/shdwmtr/wallpiper/issues/13) ([608610d](https://github.com/shdwmtr/wallpiper/commit/608610d6518df7a7a393c25583f82c8636a4ab6e))

## [1.1.1](https://github.com/shdwmtr/wallpiper/compare/v1.1.0...v1.1.1) (2026-08-24)


### Bug Fixes

* fix build system. Closes [#12](https://github.com/shdwmtr/wallpiper/issues/12) ([53f7b0b](https://github.com/shdwmtr/wallpiper/commit/53f7b0b5f350fce4c23eaedd52705ad891c5bb94))

# [1.1.0](https://github.com/shdwmtr/wallpiper/compare/v1.0.0...v1.1.0) (2026-08-24)


### Features

* **wallpiperctl:** proper CLI support, check README for documentation. Closes [#9](https://github.com/shdwmtr/wallpiper/issues/9) ([ca3810d](https://github.com/shdwmtr/wallpiper/commit/ca3810d41dc6cd41845699a27e566fa9fea37cb5))


### Performance Improvements

* remove redundant portal side monitor geometry setup ([81990f5](https://github.com/shdwmtr/wallpiper/commit/81990f513a6abaea9b0d6737d8438c202c8f9bc2))

# 1.0.0 (2026-08-24)


### Bug Fixes

* Fix gitignore ignoring source files ([1eb8e56](https://github.com/shdwmtr/wallpiper/commit/1eb8e56ae4d009c006a4bb0c953b6c879777c16b))
* fix tray menu ([64a75b2](https://github.com/shdwmtr/wallpiper/commit/64a75b2a29eb9917c79719d2f7a3b643b23eb20f))
* fix web wallpapers ([63fe4ca](https://github.com/shdwmtr/wallpiper/commit/63fe4ca5e98bacc8a4827222185c43ced21836a7))
* install KDE plugin to proper path ([285ab55](https://github.com/shdwmtr/wallpiper/commit/285ab55a30f537aaf0f2f5ea69b4b0c0ef1c9f82))
* properly reap child process ([53fb14f](https://github.com/shdwmtr/wallpiper/commit/53fb14f5fd08b8187f20a33d3f773c468e7ad09f))


### Features

* add sway support ([0cf8603](https://github.com/shdwmtr/wallpiper/commit/0cf86038f09999320c10db5dbf100fcdabaf2100))
* add wallpiper to AUR ([cddd4d9](https://github.com/shdwmtr/wallpiper/commit/cddd4d9d0880d374b74572c61613c63da5cef0b1))
* full implementation ([ae5124f](https://github.com/shdwmtr/wallpiper/commit/ae5124f2b87798030bac7a036a199040e34a5b5f))
* i3wm portal support ([6244e7f](https://github.com/shdwmtr/wallpiper/commit/6244e7f0427a972f7c5f6f193786aa9fbe15146a))
* list-wallpapers and list-properties support ([d146789](https://github.com/shdwmtr/wallpiper/commit/d1467896bfdeb0b110ad666d4dff2ce3aeee91f1))
* multi-monitor wallpaper support across all portals ([dc38d86](https://github.com/shdwmtr/wallpiper/commit/dc38d866fa2ffdd388034b53aefa29996a3a924d))
* properly scale the wallpaperui picker on hyprland, sway ([6c1edf3](https://github.com/shdwmtr/wallpiper/commit/6c1edf317ed5351b7bd8823f1317a50b08775a2c))
* reduce binary sizes ([d2fce2d](https://github.com/shdwmtr/wallpiper/commit/d2fce2d700e6ffa6b41c0a5bbecf482bddbf3ee3))
* support for COSMIC ([ee9feb8](https://github.com/shdwmtr/wallpiper/commit/ee9feb88cf1707cbcf2b8f08e767056139328b2d))
* Support for GNOME's mutter compositor ([7d4eec6](https://github.com/shdwmtr/wallpiper/commit/7d4eec6eb6a4e8dfed9ff6a9082b853be3b513f0))
* **wallpiper-portal:** Support for KDE Plasma natively with a wallpaper plugin ([96b16ef](https://github.com/shdwmtr/wallpiper/commit/96b16ef9eef9fb29355dc6e384ad20772dba4eb4))
