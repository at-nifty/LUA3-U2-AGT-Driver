# LUA3-U2-AGT-Driver

This repository provides a patched version of the Linux kernel module for the **Buffalo LUA3-U2-AGT** USB Ethernet adapter, which is based on the **ASIX AX88772B** chipset.
Filed by https://web.archive.org/web/20200907121259/http://www.asix.com.tw/products.php?op=pItemdetail&PItemID=84;71;100&PLine=71

## ✅ Features
- Plug-and-play support for LUA3-U2-AGT on modern Linux distributions
- Includes vendor/product ID patch for 0x0411:0x01A8
- DKMS compatible (optional)
- Tested on Ubuntu 24.04
- No Tested on Windows

## 📦 Installation

### Option 1: DKMS (recommended)
```bash
sudo dkms add .
sudo dkms build lua3-u2-agt/1.0
sudo dkms install lua3-u2-agt/1.0
```

### Option 2: Manual build
```bash
make
sudo insmod ./asix.ko
```

## 🧪 Verification
Check with `lsusb`:
```
Bus 001 Device 005: ID 0411:01a8 BUFFALO INC. (formerly MelCo., Inc.)
```
And confirm network interface is present:
```bash
ip link
```

## 🛠️ Patch Details
In `asix.c`, the following entry was added to the device table:
```c
/* Buffalo LUA3-U2-AGT */
{ USB_DEVICE(0x0411, 0x01A8), .driver_info = 0 },
```

## 📁 File Structure
```
lua3-u2-agt-driver/
├── README.md
├── dkms.conf
├── Makefile
├── src/
│   ├── asix.c
│   ├── asix.h
│   └── usbnet.c
├── patches/
│   └── lua3-u2-agt.patch
└── debian/
    ├── control
    └── rules
```

## ⚠️ Disclaimer
This driver is provided **as-is** without any warranty. Use at your own risk. Not an official Buffalo release.

---

## 📜 License
This project is based on Linux kernel sources and thus follows the **GPLv2** license.
