# Drivers Subsystem

**Path:** `src/drivers/`
**Headers:** `src/include/` (per-driver headers)

The drivers subsystem contains all hardware and virtual device drivers:
PCI enumeration, storage (AHCI, NVMe, ATA, virtio-blk, NBD, RAM disk),
networking (E1000, virtio-net), USB (EHCI, XHCI, HID, mass storage),
audio (AC97, OSS), display (VGA, bochs, simplefb), input (PS/2 keyboard,
mouse, USB HID), timers (HPET, PIT, TSC, RTC), ACPI, TPM 2.0, virtio
devices, DRM, GPU, and various other device classes.

## Architecture

```
PCI Bus Enumeration (pci.c — ECAM-based)
    │
    ├── Storage: ahci.c, ata.c, nvme.c, virtio_blk.c, nbd.c, ramdisk.c
    │            iSCSI (iscsi.c), NVMe-oF (nvmf.c), FCoE (fcoe.c)
    │            DRBD (drbd.c), Ceph RBD (rbd.c), bcache (bcache.c)
    │            MD RAID (mdadm.c), DM (dm*.c), device mapper stack
    │
    ├── Network: e1000.c, virtio_net.c, bonding.c
    │
    ├── USB: usb_core.c, usb_ehci.c, xhci.c, usb_hid.c, usb_msc.c,
    │         usb_serial.c, usb_eth.c, usb_cdc_ether.c, usb_cdc_acm.c
    │
    ├── Audio: ac97.c, sound_oss.c, sound_core.c, speaker.c
    │
    ├── Display: vga.c, bochs.c, simplefb.c, fbcon.c, edid.c
    │            DRM core + bochs_drm
    │
    ├── Input: ps2.c (keyboard+mouse), usb_hid.c, usb_hid_joy.c
    │
    ├── ACPI: acpi.c, acpi_ec.c, acpi_thermal.c, acpi_cpufreq.c
    │
    ├── TPM: tpm_tis.c
    │
    ├── Virtio: virtio_blk.c, virtio_net.c, virtio_scsi.c, virtio_fs.c,
    │           virtio_gpu.c, virtio_input.c, virtio_rng.c, virtio_console.c,
    │           virtio_iommu.c
    │
    ├── Virtualization: vfio.c, vhost_scsi.c, vhost_blk.c, vdpa.c,
    │                   ivshmem.c, balloon.c, vmw_balloon.c
    │
    └── Other: serial.c, rtc.c, cmos.c, watchdog.c, i2c.c, spi.c,
               hpet.c, timer.c, i6300esb.c, battery.c, dmi.c,
               pvpanic.c, firmware_class.c, coredump.c, uio.c
```

## File Descriptions

| File | Description |
|------|-------------|
| **PCI & Bus** | |
| `pci.c` | PCI/PCIe bus enumeration — ECAM-based config space access, device discovery, BAR allocation, MSI/MSI-X, SR-IOV, AER, DPC, PTM |
| `pcie_aer.c` | PCIe Advanced Error Reporting — error logging and recovery |
| `pcie_dpc.c` | PCIe Downstream Port Containment — error containment and link reset |
| `pcie_ptm.c` | PCIe Precision Time Measurement — timestamp synchronization |
| `sriov.c` | SR-IOV — Single Root I/O Virtualization, VF management |
| `iommu.c` | IOMMU — DMA remapping, interrupt remapping, device isolation |
| `dma_api.c` | DMA API — dma_alloc_coherent, streaming DMA mapping, cache-coherent buffers |
| `dma_buf.c` | DMA buffer sharing — buffer export/import between devices and subsystems |
| **Storage** | |
| `ahci.c` | AHCI — SATA controller, NCQ, port multiplier, command queuing |
| `ata.c` | ATA — legacy PATA/SATA PIO and DMA commands, identify/read/write/trim |
| `nvme.c` | NVMe — register-level controller, queue pairs, PRP/SGL, AEN, PMR |
| `nvme_pmr.c` | NVMe PMR — Persistent Memory Region management on NVMe devices |
| `virtio_blk.c` | VirtIO block — paravirtualized block device with multi-queue |
| `nbd.c` | Network Block Device — remote block device over TCP |
| `ramdisk.c` | RAM disk — memory-backed block device for initramfs/tmp |
| `loop.c` | Loop device — file-backed block device (mount file as disk) |
| `blockdev.c` | Block device abstraction — generic block I/O operations |
| `partitions.c` | Partition table parsing — MBR and GPT format support |
| `mdadm.c` | MD RAID — software RAID (RAID0/1/5/6/10), bitmap, reshape |
| `mdadm_ext.c` | MD RAID extensions — external metadata, container support |
| `bcache.c` | Bcache — block cache for slow storage using fast SSDs |
| `iscsi.c` | iSCSI initiator — session mgmt, CHAP auth, command queuing, MC/S |
| `nvmf.c` | NVMe-oF — NVMe over Fabrics target, RDMA/TCP transport |
| `fcoe.c` | FCoE — Fibre Channel over Ethernet initiator, FIP, FC-2 |
| `drbd.c` | DRBD — Distributed Replicated Block Device, sync/async replication |
| `rbd.c` | Ceph RBD — RADOS Block Device client, CRUSH placement, snapshots |
| `dm.c` | Device mapper — framework for stacking virtual block devices |
| `dm-linear.c` | DM linear — maps a range of sectors to another device |
| `dm-error.c` | DM error — returns errors on I/O (for testing) |
| `dm-zero.c` | DM zero — returns zero-filled blocks on read |
| `dm-crypt.c` | DM crypt — transparent disk encryption with LUKS |
| `dm-raid.c` | DM RAID — RAID set construction on DM targets |
| `dm-snapshot.c` | DM snapshot — copy-on-write snapshots |
| `dm-verity.c` | DM verity — block-level integrity verification with Merkle tree |
| `dm-era.c` | DM era — tracks changed blocks since a given era (thin provisioning) |
| **Network** | |
| `e1000.c` | Intel PRO/1000 — PCI/PCIe NIC, MSI-X, multi-queue RSS, interrupt moderation |
| `virtio_net.c` | VirtIO network — paravirtualized NIC, multi-queue, indirect descriptors |
| `bonding.c` | Bonding — link aggregation, 802.3ad LACP, balance-xor, active-backup, broadcast |
| `netconsole.c` | Netconsole — kernel log output over UDP network |
| `9pnet_virtio.c` | 9P2000.L — Plan 9 filesystem over virtio transport |
| **USB** | |
| `usb_core.c` | USB core — host controller abstraction, device enumeration, hub management |
| `usb_ehci.c` | EHCI — USB 2.0 Enhanced Host Controller Interface |
| `xhci.c` | xHCI — USB 3.0 eXtensible Host Controller Interface |
| `xhci_streams.c` | xHCI streams — bulk stream support for USB 3.0 mass storage |
| `usb_hub.c` | USB hub — port status, reset, speed negotiation |
| `usb_hid.c` | USB HID — Human Interface Devices (keyboard, mouse) |
| `usb_hid_joy.c` | USB HID joystick — game controller support |
| `usb_msc.c` | USB mass storage — BOT protocol, read/write |
| `usb_uas.c` | USB UAS — USB Attached SCSI protocol |
| `usb_serial.c` | USB serial — CDC ACM and generic serial adapters |
| `usb_eth.c` | USB Ethernet — CDC Ethernet and RNDIS adapters |
| `usb_cdc_ether.c` | CDC Ethernet — USB CDC Ethernet Control Model |
| `usb_cdc_acm.c` | CDC ACM — USB CDC Abstract Control Model (modems) |
| `usb_printer.c` | USB printer — printer class driver |
| `usb_typec.c` | USB Type-C — Type-C connector management, PD |
| `usb_debug.c` | USB debug — debug device for kernel debugging over USB |
| `usb_wifi.c` | USB WiFi — wireless USB adapter support |
| **Display / GPU** | |
| `vga.c` | VGA — text mode framebuffer, font loading, cursor control |
| `bochs.c` | Bochs VBE — QEMU Bochs VBE framebuffer, resolution switching |
| `simplefb.c` | Simple framebuffer — boots framebuffer handed by firmware |
| `fbcon.c` | Framebuffer console — text console on any framebuffer |
| `edid.c` | EDID — monitor EDID parsing, display modes |
| `intel_gpu.c` | Intel GPU — basic Intel integrated graphics support |
| `drm/drm_core.c` | DRM core — Direct Rendering Manager framework, modesetting |
| `drm/drm_dumb.c` | DRM dumb buffer — simple framebuffer allocation for dumb drivers |
| `drm/drm_gem.c` | DRM GEM — graphics execution manager, buffer object management |
| `drm/bochs_drm.c` | Bochs DRM — DRM driver for QEMU Bochs VGA |
| **Audio** | |
| `ac97.c` | AC97 — audio codec, PCM playback/capture, mixer controls |
| `sound_oss.c` | OSS — Open Sound System API compatibility |
| `sound_core.c` | Sound core — audio subsystem abstraction, device management |
| `sound_midi.c` | MIDI — MIDI interface support |
| `speaker.c` | PC speaker — simple tone generation via PIT |
| `sndstat.c` | Sound status — /dev/sndstat reporting |
| **Input** | |
| `ps2.c` | PS/2 — keyboard and mouse controller, AUX port, scancode translation |
| `keyboard.c` | Keyboard — keymap handling, LED control, typematic rate |
| `mouse.c` | Mouse — PS/2 mouse, scroll wheel, extra buttons |
| **Timers / RTC** | |
| `timer.c` | Timer — PIT (100 Hz) and HPET initialization, tick handler |
| `hpet.c` | HPET — High Precision Event Timer, one-shot and periodic modes |
| `rtc.c` | RTC — CMOS real-time clock, date/time get/set, alarm |
| `cmos.c` | CMOS — CMOS RAM access, NVRAM storage |
| `i6300esb.c` | i6300ESB watchdog — Intel 6300ESB I/O Controller Hub watchdog |
| **ACPI** | |
| `acpi.c` | ACPI — table parsing (RSDP, RSDT, XSDT, DSDT, SSDT), AML interpreter, device discovery |
| `acpi_ec.c` | ACPI EC — Embedded Controller communication |
| `acpi_thermal.c` | ACPI thermal — temperature zones, cooling devices, fan control |
| `acpi_cpufreq.c` | ACPI cpufreq — P-state management via ACPI _PCT/_PSS |
| `acpi_cppc.c` | ACPI CPPC — Collaborative Processor Performance Control |
| `acpi_power_button.c` | ACPI power button — power button event handling and S5 shutdown |
| `acpi_platform_profile.c` | Platform profile — performance/power/balanced profile switching |
| **TPM** | |
| `tpm_tis.c` | TPM TIS 2.0 — TPM Interface Specification 1.3 FIFO, locality mgmt, burst count |
| **Virtio** | |
| `virtio_blk.c` | VirtIO block — see Storage section |
| `virtio_net.c` | VirtIO net — see Network section |
| `virtio_scsi.c` | VirtIO SCSI — SCSI target via virtio, LUN management |
| `virtio_fs.c` | VirtIO FS — shared filesystem, FUSE-over-virtio, DAX |
| `virtio_gpu.c` | VirtIO GPU — 2D/3D acceleration, cursor, scanout |
| `virtio_input.c` | VirtIO input — keyboard, mouse, touchscreen via virtio |
| `virtio_rng.c` | VirtIO RNG — entropy source via virtio |
| `virtio_console.c` | VirtIO console — console device via virtio ports |
| `virtio_iommu.c` | VirtIO IOMMU — paravirtualized IOMMU, page table management |
| **Virtualization** | |
| `vfio.c` | VFIO — Virtual Function I/O, device groups, DMA remapping, interrupt remapping |
| `vhost_scsi.c` | Vhost SCSI — in-kernel SCSI target for virtio-scsi |
| `vhost_blk.c` | Vhost BLK — in-kernel block backend for virtio-blk |
| `vdpa.c` | vDPA — virtio Data Path Acceleration, hardware data plane offload |
| `ivshmem.c` | IVSHMEM — inter-VM shared memory PCI device |
| `balloon.c` | VirtIO balloon — dynamic guest memory management, inflate/deflate, stats |
| `vmw_balloon.c` | VMware balloon — VMware-specific memory balloon driver |
| **Other** | |
| `serial.c` | Serial — COM1/COM2 16550 UART, baud rate, IRQ-driven I/O, hardware FIFO |
| `pic.c` | PIC — Intel 8259A Programmable Interrupt Controller, remap, masking |
| `gpio.c` | GPIO — general-purpose I/O pin control, interrupt-capable pins |
| `gpio_irq.c` | GPIO IRQ — interrupt controller for GPIO pins |
| `i2c.c` | I2C — I2C bus controller, multi-master, SMBus compatibility |
| `spi.c` | SPI — Serial Peripheral Interface bus controller |
| `smbus.c` | SMBus — System Management Bus, SMBus protocol support |
| `i3c.c` | I3C — Improved Inter-Integrated Circuit bus |
| `watchdog.c` | Watchdog — /dev/watchdog interface, hardware watchdog timer |
| `firmware_class.c` | Firmware class — firmware loading framework, request_firmware API |
| `dmi.c` | DMI — Desktop Management Interface, SMBIOS table parsing, system info |
| `battery.c` | Battery — ACPI battery driver, capacity/status reporting |
| `powercap.c` | Power capping — RAPL-based power limiting, energy monitoring |
| `pmem.c` | PMEM — persistent memory (NVDIMM) block driver |
| `floppy.c` | Floppy — floppy disk controller driver |
| `edac.c` | EDAC — Error Detection and Correction, ECC syndrome decoding |
| `ghes.c` | GHES — ACPI Generic Hardware Error Source, firmware-first error handling |
| `kdb.c` | KDB — in-kernel debugger, serial console, backtrace/memory/register commands |
| `pvpanic.c` | Pvpanic — QEMU panic notification device |
| `coredump.c` | Core dump — ELF core dump generation for process crashes |
| `dyndbg.c` | Dynamic debug — runtime control of pr_debug() messages |
| `uio.c` | UIO — Userspace I/O, userspace device access framework |
| `signalfd.c` | Signalfd — file descriptor for signal delivery |
| `timerfd.c` | Timerfd — file descriptor for timer notifications |
| `eventfd.c` | Eventfd — file descriptor for event notification |
| `aio.c` | AIO — asynchronous I/O subsystem |
| `file_lock.c` | File lock — POSIX file lock management |
| `pagecache.c` | Pagecache — block device page cache operations |
| `pgrp.c` | Process group — process group management interface |
| `kaps.c` | Kernel APS — Active Protection System utility |
| `ipmi_kcs.c` | IPMI KCS — Intelligent Platform Management Interface, Keyboard Controller Style |
| `xattr.c` | Extended attributes — inode xattr operations |
| `hpet.c` | HPET — see Timers section |

## Key Conventions

- **PCI enumeration:** ECAM-based (Enhanced Configuration Access Mechanism)
  for PCIe, legacy I/O port config cycles for legacy PCI. Devices discovered
  and sorted into class-based probe order.
- **Storage drivers:** Use `blockdev` abstraction. Each driver registers a
  `struct block_device_ops` with read/write/ioctl/trim callbacks.
- **Network drivers:** Register a `struct net_device` via `netif_register()`
  with name, MAC, and transmit/receive callbacks.
- **USB:** EHCI for USB 2.0, xHCI for USB 3.0. Device enumeration creates
  interface drivers bound by class/subclass/protocol.
- **Virtio:** Uses virtio transport layer with virtqueues, feature negotiation,
  and descriptor rings. Paravirtualized devices share buffers between guest
  and host.
- **ACPI:** Tables parsed from RSDP/RSDT/XSDT. AML interpreter processes DSDT
  and SSDT for device enumeration and power management.
