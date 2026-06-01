#include "partitions.h"
#include "string.h"
#include "printf.h"

int partitions_read(const uint8_t *mbr_sector, struct partition_entry *entries) {
    if (!mbr_sector || !entries) return -1;

    /* Check MBR signature */
    const struct mbr *mbr = (const struct mbr *)mbr_sector;
    if (mbr->signature != 0xAA55) {
        /* Not a valid MBR */
        return 0;
    }

    int count = 0;
    for (int i = 0; i < 4; i++) {
        entries[i] = mbr->partitions[i];
        /* A partition entry is considered valid if type != 0 */
        if (entries[i].type != 0) count++;
    }

    return count;
}

const char *partition_type_name(uint8_t type) {
    switch (type) {
    case 0x00: return "Empty";
    case 0x01: return "FAT12";
    case 0x04: return "FAT16 (<32M)";
    case 0x05: return "Extended";
    case 0x06: return "FAT16B";
    case 0x07: return "NTFS/HPFS";
    case 0x08: return "FAT32-compatible";
    case 0x0B: return "FAT32 (CHS)";
    case 0x0C: return "FAT32 (LBA)";
    case 0x0E: return "FAT16B (LBA)";
    case 0x0F: return "Extended (LBA)";
    case 0x11: return "Hidden FAT12";
    case 0x12: return "OEM Config";
    case 0x14: return "Hidden FAT16";
    case 0x16: return "Hidden FAT16B";
    case 0x17: return "Hidden NTFS";
    case 0x1B: return "Hidden FAT32";
    case 0x1C: return "Hidden FAT32 (LBA)";
    case 0x1E: return "Hidden FAT16 (LBA)";
    case 0x27: return "Windows RE";
    case 0x39: return "Plan 9";
    case 0x41: return "PPC PReP";
    case 0x42: return "Windows LDM";
    case 0x63: return "Unix (GNU HURD)";
    case 0x80: return "Minix";
    case 0x81: return "Linux swap";
    case 0x82: return "Linux swap / Solaris";
    case 0x83: return "Linux ext2/3/4";
    case 0x84: return "OS/2 hidden C:";
    case 0x85: return "Linux extended";
    case 0x86: return "NTFS mirror set";
    case 0x87: return "NTFS stripe set";
    case 0x8E: return "Linux LVM";
    case 0x93: return "Linux software RAID (autodetect)";
    case 0x9F: return "BSD/OS";
    case 0xA0: return "IBM Thinkpad hibernation";
    case 0xA5: return "FreeBSD";
    case 0xA6: return "OpenBSD";
    case 0xA7: return "NeXTSTEP";
    case 0xA8: return "Apple UFS";
    case 0xA9: return "NetBSD";
    case 0xAB: return "Apple boot";
    case 0xAF: return "Apple HFS/HFS+";
    case 0xB7: return "BSDI FS";
    case 0xB8: return "BSDI swap";
    case 0xBE: return "Solaris boot";
    case 0xBF: return "Solaris";
    case 0xC0: return "CTOS";
    case 0xC1: return "DR-DOS FAT12";
    case 0xC4: return "DR-DOS FAT16";
    case 0xC6: return "DR-DOS FAT16B";
    case 0xC7: return "Syrinx";
    case 0xDA: return "Non-FS data";
    case 0xDE: return "Dell Utility";
    case 0xE8: return "LUKS";
    case 0xEB: return "Bech FAT16";
    case 0xEE: return "GPT protective";
    case 0xEF: return "EFI System Partition";
    case 0xF0: return "Linux PA-RISC boot";
    case 0xFB: return "VMware VMFS";
    case 0xFC: return "VMware swap";
    case 0xFD: return "Linux RAID autodetect";
    default:   return "Unknown";
    }
}
