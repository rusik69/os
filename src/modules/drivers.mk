# ── Driver modules — obj-m entries for hardware and device driver .ko files ─────
#
# This file is included by src/modules/Makefile.modules.
# Each line adds one or more driver .ko targets to the obj-m list.
# Multi-file modules define <basename>-objs := ... before their obj-m += line.
#
# Categories:
#   Core/basic  — e1000, speaker, coredump, floppy, virtio, NVMe, TPM, AHCI, USB
#   Device mapper — dm-*, ramdisk, loop, bcache, mpath
#   Virtio family — virtio_*, vhost_*, vfio, vdpa
#   Graphics     — intel_gpu, bochs, simplefb
#   Audio        — ac97, sound_*, fm_synth
#   PCIe         — pcie_aer, pcie_dpc, pcie_ptm, sriov
#   USB         — usb_uas, usb_serial, usb_cdc_ether, usb_wifi
#   Misc        — firmware_class, dma-api, sndstat, iommu, balloon, xhci, i3c,
#                 edac, ghes, spi, gpio_irq, i2c, smbus, watchdog, rtc, cmos,
#                 battery, acpi_platform_profile

# ── Core / basic drivers (default set) ──────────────────────────────────────
# If obj-m is not already set (e.g., from environment), build this baseline set.
obj-m ?= drivers/e1000.ko drivers/speaker.ko drivers/coredump.ko drivers/floppy.ko

# ── Network driver modules ──────────────────────────────────────────────────
obj-m += drivers/virtio_blk.ko drivers/virtio_net.ko
obj-m += drivers/nvme.ko
obj-m += drivers/tpm_tis.ko
obj-m += drivers/ahci.ko

# USB EHCI + Mass Storage + HID + CDC + Hub as a single module
obj-m += drivers/usb.ko
usb-objs := drivers/usb_ehci drivers/usb_msc drivers/usb_hid drivers/usb_cdc_acm drivers/usb_hub drivers/usb_transfer

# ── Additional driver modules ──────────────────────────────────────────────
# Storage / block
obj-m += drivers/ramdisk.ko
obj-m += drivers/loop.ko
obj-m += drivers/dm.ko
obj-m += drivers/dm-linear.ko
obj-m += drivers/dm-zero.ko
obj-m += drivers/dm-error.ko
obj-m += drivers/dm-crypt.ko
obj-m += drivers/dm-verity.ko
obj-m += drivers/dm-raid.ko
obj-m += drivers/dm_snapshot.ko
obj-m += drivers/dm-era.ko
obj-m += drivers/mpath.ko
obj-m += drivers/bcache.ko
obj-m += drivers/iommu.ko

# Virtio family
obj-m += drivers/virtio_gpu.ko
obj-m += drivers/virtio_input.ko
obj-m += drivers/virtio_rng.ko
obj-m += drivers/virtio_scsi.ko
obj-m += drivers/virtio_console.ko
obj-m += drivers/virtio_fs.ko
obj-m += drivers/virtio_iommu.ko
obj-m += drivers/vhost_scsi.ko
obj-m += drivers/vhost_blk.ko
obj-m += drivers/vfio.ko
obj-m += drivers/vdpa.ko
obj-m += drivers/balloon.ko
obj-m += drivers/xhci.ko

# PCIe
obj-m += drivers/pcie_aer.ko
obj-m += drivers/pcie_dpc.ko
obj-m += drivers/pcie_ptm.ko
obj-m += drivers/sriov.ko

# I3C / EDAC / GHES
obj-m += drivers/i3c.ko
obj-m += drivers/edac.ko
obj-m += drivers/ghes.ko

# Graphics
obj-m += drivers/intel_gpu.ko
obj-m += drivers/bochs.ko
obj-m += drivers/simplefb.ko

# Audio
obj-m += drivers/ac97.ko
obj-m += drivers/sound_oss.ko
obj-m += drivers/sound_core.ko
obj-m += drivers/sound_midi.ko
obj-m += drivers/fm_synth.ko

# USB sub-modules
obj-m += drivers/usb_uas.ko
obj-m += drivers/usb_serial.ko
obj-m += drivers/usb_cdc_ether.ko
obj-m += drivers/usb_wifi.ko

# Misc
obj-m += drivers/firmware_class.ko
obj-m += drivers/dma-api.ko
obj-m += drivers/sndstat.ko
obj-m += drivers/acpi_platform_profile.ko
obj-m += drivers/spi.ko
obj-m += drivers/gpio_irq.ko
obj-m += drivers/i2c.ko
obj-m += drivers/smbus.ko
obj-m += drivers/watchdog.ko
obj-m += drivers/rtc.ko
obj-m += drivers/cmos.ko
obj-m += drivers/battery.ko

# TPM key (kernel/ tree)
obj-m += kernel/tpm_key.ko
