/*
 * src/fs/iso9660_joliet.c — Joliet (UCS-2) filename decoding
 * for the ISO9660 CDROM read-only filesystem.
 *
 * Joliet is an ISO 9660 extension defined by Microsoft that allows
 * filenames to be encoded in UCS-2 Big Endian (ISO/IEC 10646),
 * enabling international (Unicode) filenames on CD-ROMs.
 *
 * Provides:
 *   - UCS-2 Big Endian to UTF-8 conversion
 *   - Joliet Supplementary Volume Descriptor (SVD) detection
 *   - Joliet root directory extraction
 */

#define KERNEL_INTERNAL
#include "iso9660.h"
#include "string.h"
#include "printf.h"

/*
 * Convert a single UCS-2 (Basic Multilingual Plane) codepoint
 * to UTF-8 encoding.
 *
 * UCS-2 encodes each character as a fixed 16-bit value covering
 * the Basic Multilingual Plane (U+0000 through U+FFFF).  Surrogate
 * pairs (U+D800-U+DFFF) are not valid UCS-2 and are not handled.
 *
 * @cp    UCS-2 codepoint value (0–0xFFFF)
 * @out   Output buffer (must have at least 4 bytes of space)
 * Returns the number of UTF-8 bytes written (1–3).
 */
int joliet_ucs2_cp_to_utf8(uint16_t cp, char *out)
{
	if (cp < 0x80) {
		/* 1-byte UTF-8: 0xxxxxxx */
		out[0] = (char)cp;
		return 1;
	} else if (cp < 0x800) {
		/* 2-byte UTF-8: 110xxxxx 10xxxxxx */
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	} else {
		/* 3-byte UTF-8: 1110xxxx 10xxxxxx 10xxxxxx */
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
}

/*
 * Convert a UCS-2 Big Endian filename (as used by Joliet) to UTF-8.
 *
 * Joliet stores filenames in UCS-2 Big Endian format (2 bytes per
 * character) without the ";1" version suffix that plain ISO 9660
 * uses.  Each directory record's name_len field gives the total byte
 * count of the UCS-2 data (always even).
 *
 * @ucs2           Pointer to UCS-2BE data (2 bytes per character)
 * @ucs2_len_bytes Length of the UCS-2 data IN BYTES (chars = ucs2_len / 2)
 * @out            Destination buffer for UTF-8 result
 * @out_max        Size of destination buffer (including NUL terminator)
 * Returns the number of characters written (excluding NUL terminator),
 *         or 0 on empty/invalid input.
 */
int joliet_ucs2be_to_utf8(const char *ucs2, int ucs2_len_bytes,
                           char *out, int out_max)
{
	if (!ucs2 || ucs2_len_bytes < 2 || !out || out_max < 2)
		return 0;

	int chars = ucs2_len_bytes / 2;
	int written = 0;

	for (int i = 0; i < chars; i++) {
		/* Read 2-byte UCS-2 Big Endian character */
		uint16_t cp = ((uint16_t)(uint8_t)ucs2[i * 2] << 8) |
		               (uint8_t)ucs2[i * 2 + 1];

		/* Skip zero bytes (padding or end of string) */
		if (cp == 0x0000)
			break;

		/*
		 * Skip the ";1" version separator that some non-compliant
		 * Joliet implementations erroneously include.  The Joliet
		 * specification explicitly says filenames shall not have
		 * version numbers, but some mastering software includes
		 * them anyway.
		 */
		if (cp == 0x003B && (i + 1 < chars)) {
			uint16_t next_cp = ((uint16_t)(uint8_t)ucs2[(i + 1) * 2] << 8) |
			                    (uint8_t)ucs2[(i + 1) * 2 + 1];
			if (next_cp == 0x0031) {
				/* ";1" found — skip it */
				break;
			}
		}

		/* Encode this UCS-2 codepoint to UTF-8 */
		int nbytes = joliet_ucs2_cp_to_utf8(cp, out + written);
		written += nbytes;
		if (written >= out_max - 1) {
			/* Truncate gracefully at buffer boundary */
			written = out_max - 1;
			break;
		}
	}

	/*
	 * Strip trailing spaces, dots, and semicolons that Joliet
	 * padding can introduce.  Some mastering tools pad filenames
	 * with spaces to fill the fixed-size directory record name area.
	 */
	while (written > 0) {
		char c = out[written - 1];
		if (c == ';' || c == ' ' || c == '.' || c == '\0')
			written--;
		else
			break;
	}

	out[written] = '\0';
	return written;
}

/*
 * Check whether a Supplementary Volume Descriptor (type 2) is a
 * Joliet SVD by examining its escape sequences.
 *
 * Joliet escape sequences appear at the beginning of the
 * escape_sequences[32] field in the SVD:
 *   "%/@", "%/C", "%/E"  = UCS-2 encoding escape sequences
 *   "%/" = 0x25 0x2F
 *
 * Returns 1 if the SVD is a valid Joliet descriptor, 0 otherwise.
 */
int joliet_is_joliet_svd(const struct iso_supplementary_desc *svd)
{
	if (!svd)
		return 0;

	/* Must have Joliet escape sequences in the first three bytes */
	if (svd->escape_sequences[0] != JOLIET_ESC_LEVEL1_0)
		return 0;
	if (svd->escape_sequences[1] != JOLIET_ESC_LEVEL1_1)
		return 0;

	/* Accept any of the three Joliet levels */
	switch (svd->escape_sequences[2]) {
	case JOLIET_ESC_LEVEL1_2: /* %/@ — level 1, no combining chars */
	case JOLIET_ESC_LEVEL2_2: /* %/C — level 2 */
	case JOLIET_ESC_LEVEL3_2: /* %/E — level 3, full UCS-2 */
		return 1;
	default:
		return 0;
	}
}

/*
 * Extract Joliet root directory information from a Supplementary
 * Volume Descriptor that has already been confirmed as a Joliet SVD
 * (via joliet_is_joliet_svd()).
 *
 * @svd         Pointer to the Supplementary Volume Descriptor
 * @extent      Output: logical block address of the root directory
 * @size        Output: size of the root directory in bytes
 * Returns 0 on success, -1 if the SVD is invalid.
 */
int joliet_get_joliet_root(const struct iso_supplementary_desc *svd,
                            uint32_t *extent, uint32_t *size)
{
	if (!svd || !extent || !size)
		return -1;

	const struct iso_dir_record *root =
	    (const struct iso_dir_record *)svd->root_dir;

	*extent = root->extent_loc_le;
	*size   = root->data_length_le;
	return 0;
}
