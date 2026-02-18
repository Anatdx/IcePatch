<div align="center">
<a href="https://github.com/Anatdx/IcePatch/releases/latest"><img src="https://images.weserv.nl/?url=https://raw.githubusercontent.com/Anatdx/IcePatch/main/app/src/main/ic_launcher-playstore.png&mask=circle" style="width: 128px;" alt="logo"></a>

<h1 align="center">IcePatch</h1>

[![Latest Release](https://img.shields.io/github/v/release/Anatdx/IcePatch?label=Release&logo=github)](https://github.com/Anatdx/IcePatch/releases/latest)
[![Group](https://img.shields.io/badge/Follow-Telegram-blue.svg?logo=telegram)](https://t.me/manosaba)
[![GitHub License](https://img.shields.io/github/license/Anatdx/IcePatch?logo=gnu)](/LICENSE)

</div>

The patching of Android kernel and Android system.

Icepatch is a fork of [Apatch](https://github.com/bmax121/APatch/)

- A new kernel-based root solution for Android devices.
- APM: Support for modules similar to Magisk.
- KPM: Support for modules that allow you to inject any code into the kernel (Provides kernel function `inline-hook` and `syscall-table-hook`).
- APatch relies on [KernelPatch](https://github.com/bmax121/KernelPatch/).
- The APatch UI and the APModule source code have been derived and modified from [KernelSU](https://github.com/tiann/KernelSU).

Download the latest APK from the [Releases Section](https://github.com/Anatdx/IcePatch/releases/latest).

## Supported Versions

- Only supports the ARM64 architecture.
- Only supports Android kernel versions 3.18 - 6.12

Support for Samsung devices with security protection: Planned

## Requirement

Kernel configs:

- `CONFIG_KALLSYMS=y` and `CONFIG_KALLSYMS_ALL=y`

- `CONFIG_KALLSYMS=y` and `CONFIG_KALLSYMS_ALL=n`: Initial support

## Security Alert

The **SuperKey** has higher privileges than root access.  
Weak or compromised keys can lead to unauthorized control of your device.  
It is critical to use robust keys and safeguard them from exposure to maintain the security of your device.

## Get Help

### Usage

For usage, please refer to [our official documentation](https://icepatch.anatdx.com).  
It's worth noting that the documentation is currently not quite complete, and the content may change at any time. 

### Discussions

- Telegram Group: [@manosaba](https://t.me/manosaba)

### More Information

- [Documents](docs/)

## Credits

- [KernelPatch](https://github.com/bmax121/KernelPatch/): The core.
- [Magisk](https://github.com/topjohnwu/Magisk): magiskboot and magiskpolicy.
- [KernelSU](https://github.com/tiann/KernelSU): App UI, and Magisk module like support.
- [SukiSU-Ultra](https://github.com/ShirkNeko/SukiSU-Ultra): UI detail styling inspiration.

## License

APatch is licensed under the GNU General Public License v3 [GPL-3](http://www.gnu.org/copyleft/gpl.html).
