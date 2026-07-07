/*
 * src/fs/iso9660_rr.c — Rock Ridge (RRIP) extension helpers
 * for the ISO9660 CDROM read-only filesystem.
 *
 * Provides:
 *   - ISO 9660 7-byte timestamp → Unix time_t conversion
 *   - TF (Timestamps) SUSP entry parsing
 *   - PX helper for applying POSIX attributes to VFS stat
 *   - PN helper for applying device node info to VFS stat
 */

#define KERNEL_INTERNAL
#include "iso9660.h"
#include "printf.h"
#include "string.h"

/* ── Date to Unix epoch conversion ─────────────────────────────── */

/* Days in each month for non-leap and leap years */
static const uint16_t __month_days[2][12] = {
	{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
	{31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}
};

/* Is @year a leap year?  Years are in full 4-digit form (e.g. 1970). */
static int __is_leap_year(uint32_t year)
{
	return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

/*
 * Convert ISO 9660 7-byte timestamp to Unix time_t
 * (seconds since 1970-01-01 00:00:00 UTC).
 *
 * ISO 9660 timestamp format (7 bytes):
 *   [0] year    – years since 1900   (0..255)
 *   [1] month   – 1–12
 *   [2] day     – 1–31
 *   [3] hour    – 0–23
 *   [4] minute  – 0–59
 *   [5] second  – 0–59
 *   [6] gmt_off – signed 15-min offsets from GMT (-48..+52)
 *
 * Returns 0 on success, -1 on invalid date.
 */
int iso9660_rr_decode_timestamp(const uint8_t iso_time[7], uint32_t *out_time)
{
	uint32_t year, month, day, hour, minute, second;
	int gmt_off_signed;
	int32_t gmt_offset;

	if (!iso_time || !out_time)
		return -1;

	year   = (uint32_t)iso_time[0] + 1900;
	month  = (uint32_t)iso_time[1];
	day    = (uint32_t)iso_time[2];
	hour   = (uint32_t)iso_time[3];
	minute = (uint32_t)iso_time[4];
	second = (uint32_t)iso_time[5];

	/* Validate range */
	if (month < 1 || month > 12)
		return -1;
	if (day < 1 || day > 31)
		return -1;
	if (hour > 23 || minute > 59 || second > 59)
		return -1;

	/* Count days from 1970-01-01 to year-month-day */
	uint32_t days = 0;

	/* Full years 1970 .. year-1 */
	for (uint32_t y = 1970; y < year; y++)
		days += __is_leap_year(y) ? 366 : 365;

	/* Days in current year up to month-1 */
	int leap = __is_leap_year(year);
	for (uint32_t m = 1; m < month; m++)
		days += (uint32_t)__month_days[leap][m - 1];

	/* Days in current month (day-1 because day 1 = 0 elapsed days) */
	days += (day - 1);

	/* Convert to seconds */
	uint32_t total = days * 86400 + hour * 3600 + minute * 60 + second;

	/* Apply GMT offset (GMT offset is in 15-minute intervals, signed).
	 * The ISO 9660 field is a signed byte stored as uint8_t.
	 * Values 0..63 are +0..+63; values 64..255 are negative:
	 *   value 64 → 0, 65 → -1, ..., 255 → -191
	 * We only handle the common range 0..52 (0..+780 min = 0..+13h). */
	gmt_off_signed = (int)iso_time[6];
	if (gmt_off_signed >= 64)
		gmt_offset = -(gmt_off_signed - 64);
	else
		gmt_offset = gmt_off_signed;

	/* Subtract GMT offset to get UTC (if +2h offset, subtract 7200s) */
	total -= (uint32_t)(gmt_offset * 900u);  /* 15-minute units */

	*out_time = total;
	return 0;
}

/* ── TF (Timestamps) entry parsing ─────────────────────────────── */

/*
 * Parse a Rock Ridge TF entry.
 *
 * The TF entry follows the SUSP format and contains a flags byte followed
 * by 7-byte ISO 9660 timestamps for each flag bit that is set.  Per the
 * RRIP spec, only timestamps with set flags are physically present in the
 * data, in order of their flag bit position (0 = creation, 1 = modify,
 * 2 = access, 3 = attrib change, 4 = backup, 5 = expiration, 6 = effective).
 *
 * Returns a bitmask of RRIP_TF_* flags indicating which timestamps were
 * successfully parsed.
 */
uint8_t iso9660_rr_parse_tf(const struct rrip_tf_entry *tf, uint32_t tf_len,
                             uint32_t *atime, uint32_t *mtime, uint32_t *ctime,
                             uint32_t *btime)
{
	uint8_t flags = 0;
	uint32_t offset = 5; /* skip header (4) + flags (1) */
	uint32_t parsed = 0;

	if (!tf || tf_len < 5 || !atime || !mtime || !ctime)
		return 0;

	uint8_t tf_flags = tf->flags;

	/* TF timestamp order (per SUSP/RRIP spec):
	 * Only timestamps with set flags appear in the data stream,
	 * in order of their flag bit position:
	 *   bit 0: creation time    (stored in btime)
	 *   bit 1: modification time (stored in mtime)
	 *   bit 2: access time       (stored in atime)
	 *   bit 3: attribute change  (stored in ctime)
	 *   bit 4: backup time       (consumed, not stored)
	 *   bit 5: expiration time   (consumed, not stored)
	 *   bit 6: effective time    (consumed, not stored)
	 */
	for (int i = 0; i < 7; i++) {
		if (!(tf_flags & (1 << i))) {
			/* Timestamp not present — per RRIP spec, only
			 * timestamps with set flag bits are physically
			 * present, so we skip without advancing offset. */
			continue;
		}

		if (offset + 7 > tf_len)
			break; /* truncated */

		const uint8_t *ts = &tf->timestamps[offset - 5];
		uint32_t ts_val = 0;

		if (iso9660_rr_decode_timestamp(ts, &ts_val) == 0) {
			switch (i) {
			case 0: /* creation time (birth time) */
				if (btime) {
					*btime = ts_val;
					flags |= RRIP_TF_CREATE;
					parsed++;
				}
				break;
			case 1: /* modify time */
				*mtime = ts_val;
				flags |= RRIP_TF_MODIFY;
				parsed++;
				break;
			case 2: /* access time */
				*atime = ts_val;
				flags |= RRIP_TF_ACCESS;
				parsed++;
				break;
			case 3: /* attribute change time */
				*ctime = ts_val;
				flags |= RRIP_TF_ATTRIB;
				parsed++;
				break;
			default:
				/* backup(4), expire(5), effective(6) —
				 * consumed but not stored in outputs */
				break;
			}
		}
		offset += 7;
	}

	if (parsed > 0)
		return flags;

	return 0;
}

/* ── PX helper ─────────────────────────────────────────────────── */

/*
 * Apply Rock Ridge PX attributes to a vfs_stat structure.
 *
 * Fills in mode, uid, gid, nlink, atime, mtime from the RRIP entry
 * when PX was present.  The RRIP entry's timestamp fields already
 * reflect the best available source (TF takes precedence over PX
 * because TF is parsed after PX in the SUSP walk and overwrites
 * the PX values).
 *
 * Returns 0 on success, -1 if no PX data.
 */
int iso9660_rr_apply_px(const struct iso_rrip_entry *de, struct vfs_stat *st)
{
	if (!de || !st)
		return -1;

	if (!(de->rr_flags & RRIP_HAS_PX))
		return -1;

	st->mode  = de->rr_mode & 07777;
	st->uid   = (uint16_t)de->rr_uid;
	st->gid   = (uint16_t)de->rr_gid;
	st->nlink = de->rr_nlink ? de->rr_nlink : 1;

	/* Timestamp fields in the rrip entry already have TF values
	 * overlaying PX values (TF is parsed after PX in the SUSP walk).
	 * Use them directly — no conditional needed. */
	st->atime = de->rr_atime;
	st->mtime = de->rr_mtime;

	return 0;
}

/* ── PN helper ─────────────────────────────────────────────────── */

/*
 * Apply Rock Ridge PN (POSIX Device Node) entry to a vfs_stat structure.
 *
 * Fills in dev_major and dev_minor from the RRIP entry when PN was
 * present.  Rock Ridge represents the device number as a (high, low)
 * pair in the PN SUSP entry, where high = major and low = minor.
 */
void iso9660_rr_apply_pn(const struct iso_rrip_entry *de, struct vfs_stat *st)
{
	if (!de || !st)
		return;

	if (!(de->rr_flags & RRIP_HAS_PN))
		return;

	st->dev_major = (uint16_t)de->rr_dev_major;
	st->dev_minor = (uint16_t)de->rr_dev_minor;
}
