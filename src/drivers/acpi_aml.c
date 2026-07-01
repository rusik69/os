/*
 * acpi_aml.c — ACPI AML bytecode namespace construction
 *
 * Walks DSDT and SSDT definition blocks to build the ACPI namespace tree.
 * Each named object (Scope, Device, Processor, PowerResource, ThermalZone,
 * Name, Method) becomes a node in the namespace, linked via parent/first_child/
 * next_sibling pointers (flat array storage).
 *
 * The namespace is the foundation for AML method evaluation, device discovery,
 * and ACPI power management.
 */

#include "acpi.h"
#include "string.h"
#include "printf.h"

/* ── AML Opcodes (subset relevant to namespace construction) ────── */

#define AML_ZERO_OP             0x00
#define AML_ONE_OP              0x01
#define AML_ALIAS_OP            0x06
#define AML_NAME_OP             0x08
#define AML_BYTE_PREFIX         0x0A
#define AML_WORD_PREFIX         0x0B
#define AML_DWORD_PREFIX        0x0C
#define AML_STRING_PREFIX       0x0D
#define AML_QWORD_PREFIX        0x0E
#define AML_SCOPE_OP            0x10
#define AML_BUFFER_OP           0x11
#define AML_PACKAGE_OP          0x12
#define AML_VAR_PACKAGE_OP      0x13
#define AML_METHOD_OP           0x14
#define AML_DUAL_NAME_PREFIX    0x2E
#define AML_MULTI_NAME_PREFIX   0x2F
#define AML_EXT_OP_PREFIX       0x5B
#define AML_ROOT_PREFIX          0x5C
#define AML_PARENT_PREFIX        0x5E

/* Extended opcodes (prefix 0x5B) */
#define AML_EXT_OPREGION_OP     0x80
#define AML_EXT_FIELD_OP        0x81
#define AML_EXT_DEVICE_OP       0x82
#define AML_EXT_PROCESSOR_OP    0x83
#define AML_EXT_POWERRESOURCE_OP 0x84
#define AML_EXT_THERMAL_ZONE_OP 0x85
#define AML_EXT_INDEX_FIELD_OP  0x86
#define AML_EXT_BANK_FIELD_OP   0x87

/* Data type names used by AML NameOp values */
#define AML_DATA_TYPE_INTEGER   0
#define AML_DATA_TYPE_STRING    1
#define AML_DATA_TYPE_BUFFER    2
#define AML_DATA_TYPE_PACKAGE   3

/* ── Namespace Storage ──────────────────────────────────────────── */

/* The flat namespace array. We use a static pool sized generously. */
static struct {
	int              count;
	struct aml_ns_node nodes[ACPI_NS_MAX_NODES];
} g_ns;

/* Parser state during namespace construction */
struct aml_parse_state {
	const uint8_t *aml;         /* Current AML bytecode pointer */
	uint32_t       aml_len;     /* Total length of AML */
	uint32_t       offset;      /* Current offset into AML */
	uint16_t       parent;      /* Current parent node index (0xFFFF = root) */
	uint8_t        ssdt_index;  /* Which SSDT we are parsing (0 = DSDT) */
};

/* ── PkgLength Decoder ──────────────────────────────────────────── */

/*
 * Decode an AML PkgLength from the byte stream.
 *
 * ACPI spec (v6.3, section 5.4): The PkgLength is encoded in 1-4 bytes.
 * If lead byte bit 7 = 0: length = lead_byte & 0x3F (0-63), 1 byte.
 * If lead byte bit 7 = 1: number of follow bytes = (lead >> 4) & 0x3.
 *   Length = lead & 0x0F, then each follow byte provides 8 more bits.
 *
 * Returns the decoded length.  Sets *bytes_used to the number of bytes
 * consumed from the stream (1-4).  Returns 0 on error (offset out of range).
 */
static uint32_t aml_decode_pkg_length(const uint8_t *data, uint32_t max_len,
				      uint32_t offset, uint32_t *bytes_used)
{
	uint32_t result = 0;

	if (bytes_used)
		*bytes_used = 0;

	if (offset >= max_len)
		return 0;

	uint8_t lead = data[offset];

	if (lead & 0x80) {
		/* Multi-byte: bits [6:4] = number of follow bytes (0-3) */
		uint32_t num_follow = (lead >> 4) & 0x3;
		uint32_t total = 1 + num_follow;

		if (offset + total > max_len)
			return 0;

		result = lead & 0x0F;
		for (uint32_t i = 1; i < total; i++)
			result |= (uint32_t)data[offset + i] << (4 + (i - 1) * 8);

		if (bytes_used)
			*bytes_used = total;
	} else {
		/* Single byte: bits [5:0] = length */
		result = lead & 0x3F;
		if (bytes_used)
			*bytes_used = 1;
	}

	return result;
}

/* ── NameSeg Parsing ────────────────────────────────────────────── */

/*
 * Read a 4-byte NameSeg from the byte stream.  AML names are always
 * exactly 4 characters, padded with underscores ('_' = 0x5F).
 * Returns the number of bytes consumed (always 4).
 */
static int aml_read_nameseg(const uint8_t *data, uint32_t max_len,
			    uint32_t offset, char name[4])
{
	if (offset + 4 > max_len) {
		name[0] = '?';
		name[1] = '?';
		name[2] = '?';
		name[3] = '\0';
		return 0;
	}

	memcpy(name, &data[offset], 4);
	return 4;
}

/*
 * Read a NameString from the byte stream.  A NameString can be:
 *   - Single NameSeg (4 bytes)
 *   - DualNamePrefix (0x2E) + NameSeg + NameSeg (9 bytes)
 *   - MultiNamePrefix (0x2F) + count + NameSeg * count
 *   - RootPrefix (0x5C) + NameString
 *   - ParentPrefix (0x5E) + NameString
 *
 * We read past the NameString and return the total bytes consumed.
 * The resolved path is not returned (namespace construction resolves
 * paths through the parent scope chain).  Returns bytes consumed, or
 * 0 on error.
 */
static uint32_t aml_skip_name_string(const uint8_t *data, uint32_t max_len,
				     uint32_t offset)
{
	uint32_t o = offset;

	if (o >= max_len)
		return 0;

	/* Handle root and parent prefixes */
	while (o < max_len) {
		if (data[o] == AML_ROOT_PREFIX) {
			o++;
		} else if (data[o] == AML_PARENT_PREFIX) {
			o++;
		} else {
			break;
		}
	}

	if (o >= max_len)
		return 0;

	/* Now at the NameSeg / prefix */
	if (o + 1 > max_len)
		return 0;

	uint8_t prefix = data[o];

	if (prefix == AML_DUAL_NAME_PREFIX) {
		/* 0x2E + NameSeg + NameSeg = 1 + 4 + 4 = 9 bytes */
		if (o + 9 > max_len)
			return 0;
		return (o + 9) - offset;
	} else if (prefix == AML_MULTI_NAME_PREFIX) {
		/* 0x2F + count + NameSeg * count */
		if (o + 2 > max_len)
			return 0;
		uint8_t count = data[o + 1];

		if (count == 0)
			return 0;
		uint32_t total = 2 + (uint32_t)count * 4;

		if (o + total > max_len)
			return 0;
		return (o + total) - offset;
	} else {
		/* Single NameSeg (4 bytes) */
		if (o + 4 > max_len)
			return 0;
		return (o + 4) - offset;
	}
}

/*
 * Extract the final NameSeg (last 4-byte segment) from a NameString.
 * Returns bytes consumed from the stream, or 0 on error.
 * The NameSeg is written to 'name'.
 */
static uint32_t aml_read_last_nameseg(const uint8_t *data, uint32_t max_len,
				      uint32_t offset, char name[4])
{
	uint32_t o = offset;
	uint32_t last_seg_off = offset;
	int has_name = 0;

	if (o >= max_len)
		return 0;

	/* Skip root/parent prefixes */
	while (o < max_len) {
		if (data[o] == AML_ROOT_PREFIX) {
			o++;
		} else if (data[o] == AML_PARENT_PREFIX) {
			o++;
		} else {
			break;
		}
	}

	if (o >= max_len)
		return 0;

	/* Walk through the NameString to find the last NameSeg */
	while (o < max_len) {
		if (o + 1 > max_len)
			break;

		if (data[o] == AML_DUAL_NAME_PREFIX) {
			/* Two NameSegs — the second is the last */
			if (o + 9 > max_len)
				return 0;
			last_seg_off = o + 5;  /* skip prefix (1) + first seg (4) */
			o += 9;
			has_name = 1;
		} else if (data[o] == AML_MULTI_NAME_PREFIX) {
			if (o + 2 > max_len)
				return 0;
			uint8_t count = data[o + 1];

			if (count == 0)
				return 0;
			/* Last NameSeg is at (count-1)*4 bytes after the count byte */
			last_seg_off = o + 2 + (uint32_t)(count - 1) * 4;
			if (last_seg_off + 4 > max_len)
				return 0;
			o += 2 + (uint32_t)count * 4;
			has_name = 1;
			break;
		} else {
			/* Single NameSeg */
			if (o + 4 > max_len)
				return 0;
			last_seg_off = o;
			o += 4;
			has_name = 1;
			break;
		}
	}

	if (!has_name) {
		memcpy(name, "????", 4);
		return 0;
	}

	memcpy(name, &data[last_seg_off], 4);
	return o - offset;
}

/* ── Namespace Node Management ──────────────────────────────────── */

/*
 * Add a new namespace node.  Returns the index of the new node, or
 * -1 if the namespace array is full or parameters are invalid.
 */
static int aml_add_node(const char name[4], uint8_t type, uint16_t parent,
			const uint8_t *aml_start, uint32_t aml_length,
			uint8_t ssdt_index)
{
	if (g_ns.count >= ACPI_NS_MAX_NODES) {
		kprintf("[AML] Namespace full (%d nodes max)\n", ACPI_NS_MAX_NODES);
		return -1;
	}

	int idx = g_ns.count;
	struct aml_ns_node *node = &g_ns.nodes[idx];

	memcpy(node->name, name, 4);
	node->type = type;
	node->parent = parent;
	node->first_child = 0xFFFF;
	node->next_sibling = 0xFFFF;
	node->aml_start = (uint8_t *)aml_start;
	node->aml_length = aml_length;
	node->from_ssdt = ssdt_index;

	g_ns.count++;

	/* Link as child of parent */
	if (parent != 0xFFFF && parent < g_ns.count) {
		struct aml_ns_node *p = &g_ns.nodes[parent];

		if (p->first_child == 0xFFFF) {
			p->first_child = (uint16_t)idx;
		} else {
			/* Find last sibling and link */
			int sib = p->first_child;

			while (sib != 0xFFFF && g_ns.nodes[sib].next_sibling != 0xFFFF)
				sib = g_ns.nodes[sib].next_sibling;
			if (sib != 0xFFFF)
				g_ns.nodes[sib].next_sibling = (uint16_t)idx;
		}
	}

	return idx;
}

/*
 * Check if a node already exists with the given name and parent.
 * Returns the node index, or -1 if not found.
 */
static int aml_find_child(uint16_t parent, const char name[4])
{
	if (parent == 0xFFFF)
		return -1;

	struct aml_ns_node *p = &g_ns.nodes[parent];
	int child = p->first_child;

	while (child != 0xFFFF) {
		if (memcmp(g_ns.nodes[child].name, name, 4) == 0)
			return child;
		child = g_ns.nodes[child].next_sibling;
	}

	return -1;
}

/* ── Forward declarations ──────────────────────────────────────── */
static uint32_t aml_walk_terms(struct aml_parse_state *state,
			       uint32_t max_offset);

/* ── Scope/Device/Name Handler ──────────────────────────────────── */

/*
 * Process a named object in the AML bytecode.  Different opcodes
 * create different types of namespace nodes:
 *
 *   ScopeOp (0x10):      PkgLength, NameString, TermList
 *   DeviceOp (0x5B 0x82): PkgLength, NameString, TermList
 *   ProcessorOp (0x5B 0x83): PkgLength, NameString, ProcID, PblkAddr, PblkLen, TermList
 *   PowerResourceOp (0x5B 0x84): PkgLength, NameString, SystemLevel, ResourceOrder, TermList
 *   ThermalZoneOp (0x5B 0x85): PkgLength, NameString, TermList
 *   NameOp (0x08):       NameString, DataRefObject
 *   MethodOp (0x14):     PkgLength, NameString, MethodFlags, TermList
 *   AliasOp (0x06):      NameString, NameString
 *
 * Returns the number of bytes consumed from AML, or 0 on error.
 */
static uint32_t aml_process_object(struct aml_parse_state *state,
				   uint8_t opcode, uint8_t ext_opcode)
{
	uint32_t o = state->offset;
	uint32_t max_len = state->aml_len;
	const uint8_t *aml = state->aml;
	uint16_t parent = state->parent;

	if (o >= max_len)
		return 0;

	uint32_t pkg_len = 0;
	uint32_t pkg_bytes = 0;
	uint8_t node_type;
	char name[4];
	uint32_t name_bytes;
	uint32_t body_offset = 0;
	uint32_t total_size = 0;
	int is_named_object = 0;
	int is_container = 0;

	/*
	 * Determine the node type and parse the structure based on opcode.
	 * Most namespace-creating opcodes follow:
	 *   <opcode> [<ext_opcode>] <PkgLength> <NameString> <body>
	 */
	switch (opcode) {
	case AML_SCOPE_OP:
		node_type = AML_NS_SCOPE;
		is_named_object = 1;
		is_container = 1;
		/* ScopeOp PkgLength NameString TermList */
		o++;  /* skip opcode */
		pkg_len = aml_decode_pkg_length(aml, max_len, o, &pkg_bytes);
		if (pkg_len == 0)
			return 0;
		o += pkg_bytes;
		name_bytes = aml_read_last_nameseg(aml, max_len, o, name);
		if (name_bytes == 0)
			return 0;
		body_offset = o + name_bytes;
		total_size = 1 + pkg_bytes + pkg_len;
		break;

	case AML_METHOD_OP:
		node_type = AML_NS_METHOD;
		is_named_object = 1;
		is_container = 0;
		/* MethodOp PkgLength NameString MethodFlags TermList */
		o++;
		pkg_len = aml_decode_pkg_length(aml, max_len, o, &pkg_bytes);
		if (pkg_len == 0)
			return 0;
		o += pkg_bytes;
		name_bytes = aml_read_last_nameseg(aml, max_len, o, name);
		if (name_bytes == 0)
			return 0;
		body_offset = o + name_bytes;
		total_size = 1 + pkg_bytes + pkg_len;
		break;

	case AML_NAME_OP:
		node_type = AML_NS_NAME;
		is_named_object = 1;
		is_container = 0;
		/* NameOp NameString DataRefObject */
		o++;
		name_bytes = aml_skip_name_string(aml, max_len, o);
		if (name_bytes == 0)
			return 0;
		/* Read the NameSeg */
		{
			char tmp_name[4];
			uint32_t nbo = o;

			aml_read_last_nameseg(aml, max_len, nbo, tmp_name);
			memcpy(name, tmp_name, 4);
		}
		body_offset = o + name_bytes;
		/* Calculate data length: we don't know it ahead of time,
		 * but we can skip the DataRefObject by decoding it. */
		total_size = 1 + name_bytes;
		{
			/* Skip the DataRefObject bytes */
			uint32_t dob = body_offset;

			if (dob < max_len) {
				uint8_t dtype = aml[dob];

				if (dtype == AML_BYTE_PREFIX && dob + 2 <= max_len) {
					dob += 2;
				} else if (dtype == AML_WORD_PREFIX && dob + 3 <= max_len) {
					dob += 3;
				} else if (dtype == AML_DWORD_PREFIX && dob + 5 <= max_len) {
					dob += 5;
				} else if (dtype == AML_QWORD_PREFIX && dob + 9 <= max_len) {
					dob += 9;
				} else if (dtype == AML_STRING_PREFIX) {
					dob++;
					while (dob < max_len && aml[dob] != 0)
						dob++;
					if (dob < max_len)
						dob++;
				} else if (dtype == AML_BUFFER_OP && dob + 2 <= max_len) {
					uint32_t buf_bytes;
					uint32_t buf_len = aml_decode_pkg_length(
						aml, max_len, dob + 1, &buf_bytes);

					dob += 1 + buf_bytes + buf_len;
				} else if (dtype == AML_PACKAGE_OP && dob + 2 <= max_len) {
					uint32_t pkg_bytes2;
					uint32_t pkg_len2 = aml_decode_pkg_length(
						aml, max_len, dob + 1, &pkg_bytes2);

					dob += 1 + pkg_bytes2 + pkg_len2;
				} else {
					/* ZeroOp, OneOp, Ones, or integer — just advance 1 */
					dob++;
				}
				total_size = dob - (o - 1);
			}
		}
		break;

	case AML_ALIAS_OP:
		/* Alias doesn't create a namespace node in our tree,
		 * skip both NameStrings */
		o++;
		name_bytes = aml_skip_name_string(aml, max_len, o);
		if (name_bytes == 0)
			return 0;
		o += name_bytes;
		name_bytes = aml_skip_name_string(aml, max_len, o);
		if (name_bytes == 0)
			return 0;
		total_size = (o + name_bytes) - (o - 1 - name_bytes);
		return total_size;

	case AML_EXT_OP_PREFIX:
		/* Extended opcode: the real opcode is in ext_opcode */
		switch (ext_opcode) {
		case AML_EXT_DEVICE_OP:
			node_type = AML_NS_DEVICE;
			is_named_object = 1;
			is_container = 1;
			o += 2;  /* skip EXT_PREFIX + device opcode */
			pkg_len = aml_decode_pkg_length(aml, max_len, o, &pkg_bytes);
			if (pkg_len == 0)
				return 0;
			o += pkg_bytes;
			name_bytes = aml_read_last_nameseg(aml, max_len, o, name);
			if (name_bytes == 0)
				return 0;
			body_offset = o + name_bytes;
			total_size = 2 + pkg_bytes + pkg_len;
			break;

		case AML_EXT_PROCESSOR_OP:
			node_type = AML_NS_PROCESSOR;
			is_named_object = 1;
			is_container = 1;
			o += 2;  /* skip EXT_PREFIX + processor opcode */
			pkg_len = aml_decode_pkg_length(aml, max_len, o, &pkg_bytes);
			if (pkg_len == 0)
				return 0;
			o += pkg_bytes;
			name_bytes = aml_read_last_nameseg(aml, max_len, o, name);
			if (name_bytes == 0)
				return 0;
			body_offset = o + name_bytes;
			total_size = 2 + pkg_bytes + pkg_len;
			break;

		case AML_EXT_POWERRESOURCE_OP:
			node_type = AML_NS_POWERRESOURCE;
			is_named_object = 1;
			is_container = 1;
			o += 2;
			pkg_len = aml_decode_pkg_length(aml, max_len, o, &pkg_bytes);
			if (pkg_len == 0)
				return 0;
			o += pkg_bytes;
			name_bytes = aml_read_last_nameseg(aml, max_len, o, name);
			if (name_bytes == 0)
				return 0;
			body_offset = o + name_bytes;
			total_size = 2 + pkg_bytes + pkg_len;
			break;

		case AML_EXT_THERMAL_ZONE_OP:
			node_type = AML_NS_THERMAL_ZONE;
			is_named_object = 1;
			is_container = 1;
			o += 2;
			pkg_len = aml_decode_pkg_length(aml, max_len, o, &pkg_bytes);
			if (pkg_len == 0)
				return 0;
			o += pkg_bytes;
			name_bytes = aml_read_last_nameseg(aml, max_len, o, name);
			if (name_bytes == 0)
				return 0;
			body_offset = o + name_bytes;
			total_size = 2 + pkg_bytes + pkg_len;
			break;

		default:
			/* Unknown extended opcode — skip 2 bytes (prefix + ext) and return */
			return 2;
		}
		break;

	default:
		return 0;  /* Unknown opcode, caller will handle */
	}

	if (!is_named_object || total_size == 0)
		return total_size;

	/*
	 * Check for duplicate names under the same parent.
	 * If the node already exists, skip adding it (SSDT may extend
	 * a scope already defined in DSDT).
	 */
	int existing = aml_find_child(parent, name);

	if (existing >= 0) {
		if (is_container) {
			/* If the existing node is a container (Scope, Device),
			 * we need to recurse into the body to pick up any
			 * additional definitions. */
		}
		return total_size;
	}

	/* Add the namespace node */
	int node_idx = aml_add_node(name, node_type, parent,
				    &aml[o - 1], total_size, state->ssdt_index);

	if (node_idx < 0)
		return total_size;

	/* If this is a container (Scope, Device, etc.), recurse into body */
	if (is_container) {
		uint32_t body_len = total_size;

		if (opcode == AML_EXT_OP_PREFIX) {
			/* Body is after: ext_prefix(1) + ext_op(1) + pkg_len + name */
			body_len = pkg_len;
		} else {
			body_len = pkg_len;
		}

		/*
		 * The body starts after: opcode + pkg_length_bytes + name_string.
		 * body_len is the PkgLength which includes everything except the
		 * opcode and length encoding bytes.
		 */
		uint32_t body_start = body_offset;
		uint32_t body_end = body_start + body_len;

		if (body_end > max_len)
			body_end = max_len;

		/* Save and restore parse state for recursion */
		struct aml_parse_state saved = *state;

		state->parent = (uint16_t)node_idx;
		state->offset = body_start;

		/* Walk the body TermList */
		while (state->offset < body_end) {
			uint32_t consumed;

			consumed = aml_walk_terms(state, body_end);
			if (consumed == 0)
				break;
			state->offset += consumed;
		}

		*state = saved;
	}

	return total_size;
}

/* ── AML Term Walker ────────────────────────────────────────────── */

/*
 * Walk AML terms starting at state->offset until max_offset.
 * Processes namespace-creating objects (Scope, Device, Name, Method, etc.)
 * and skips over non-namespace terms (arithmetic, control flow, etc.).
 *
 * Returns the number of bytes consumed for the processed term, or 0 on error.
 */
static uint32_t aml_walk_terms(struct aml_parse_state *state, uint32_t max_offset)
{
	uint32_t o = state->offset;
	const uint8_t *aml = state->aml;

	if (o >= state->aml_len || o >= max_offset)
		return 0;

	uint8_t opcode = aml[o];

	switch (opcode) {
	/* ── Namespace-creating opcodes ────────────────────────── */
	case AML_SCOPE_OP:
	case AML_METHOD_OP:
	case AML_NAME_OP:
	case AML_ALIAS_OP:
		return aml_process_object(state, opcode, 0);

	/* ── Extended opcodes (0x5B prefix) ────────────────────── */
	case AML_EXT_OP_PREFIX:
		if (o + 1 >= state->aml_len)
			return 1;
		return aml_process_object(state, AML_EXT_OP_PREFIX, aml[o + 1]);

	/* ── Namespace-creating dual-opcode sequences ───────────── */

	/* ── Data objects (no namespace effect) ─────────────────── */
	case AML_ZERO_OP:
	case AML_ONE_OP:
		return 1;

	case AML_BYTE_PREFIX:
		return (o + 2 <= state->aml_len) ? 2 : 0;
	case AML_WORD_PREFIX:
		return (o + 3 <= state->aml_len) ? 3 : 0;
	case AML_DWORD_PREFIX:
		return (o + 5 <= state->aml_len) ? 5 : 0;
	case AML_QWORD_PREFIX:
		return (o + 9 <= state->aml_len) ? 9 : 0;
	case AML_STRING_PREFIX:
	{
		uint32_t so = o + 1;

		while (so < state->aml_len && aml[so] != 0)
			so++;
		if (so < state->aml_len)
			so++;  /* skip null terminator */
		return so - o;
	}

	/* ── Package/Buffer ─────────────────────────────────────── */
	case AML_PACKAGE_OP:
	case AML_VAR_PACKAGE_OP:
	case AML_BUFFER_OP:
	{
		uint32_t pkg_bytes;
		uint32_t pkg_len = aml_decode_pkg_length(aml, state->aml_len,
							  o + 1, &pkg_bytes);

		if (pkg_len == 0)
			return 1;
		return 1 + pkg_bytes + pkg_len;
	}

	/* ── Control flow ───────────────────────────────────────── */
	default:
	{
		/*
		 * For unknown opcodes, we make a best-effort attempt
		 * to skip them by computing their total size.
		 *
		 * Most AML terms that carry sub-trees use PkgLength
		 * encoding.  We try to parse as:
		 *   <opcode> <PkgLength> <body>
		 */
		if (opcode >= 0x11 && opcode <= 0x2D) {
			/* Many opcodes in this range use PkgLength */
			if (o + 1 < state->aml_len) {
				uint32_t pkg_bytes;
				uint32_t pkg_len;

				pkg_len = aml_decode_pkg_length(aml, state->aml_len,
								o + 1, &pkg_bytes);
				if (pkg_len > 0)
					return 1 + pkg_bytes + pkg_len;
			}
		}

		/*
		 * If we can't decode, just skip 1 byte to avoid
		 * infinite loops.  This may cause misalignment but
		 * prevents hangs.
		 */
		return 1;
	}
	}
}

/* ── Main Namespace Builder ─────────────────────────────────────── */

int aml_build_namespace(void)
{
	/* Reset namespace */
	memset(&g_ns, 0, sizeof(g_ns));

	/* Create root node */
	{
		char root_name[4] = { '\\', '_', '_', '_' };
		int root_idx = aml_add_node(root_name, AML_NS_ROOT, 0xFFFF, NULL, 0, 0);

		if (root_idx < 0) {
			kprintf("[AML] Failed to create root namespace node\n");
			return -1;
		}

		/* Process DSDT AML */
		if (g_dsdt_aml_base != NULL && g_dsdt_aml_length > 0) {
			struct aml_parse_state state;

			memset(&state, 0, sizeof(state));
			state.aml = g_dsdt_aml_base;
			state.aml_len = g_dsdt_aml_length;
			state.offset = 0;
			state.parent = (uint16_t)root_idx;
			state.ssdt_index = 0;

			kprintf("[AML] Building namespace from DSDT (%u bytes AML)\n",
				(unsigned int)g_dsdt_aml_length);

			while (state.offset < state.aml_len) {
				uint32_t consumed = aml_walk_terms(&state, state.aml_len);

				if (consumed == 0) {
					kprintf("[AML] Warning: parse stuck at DSDT offset %u\n",
						(unsigned int)state.offset);
					break;
				}
				state.offset += consumed;
			}
		}

		/* Process SSDT AML tables */
		for (int i = 0; i < g_acpi_ssdt_count; i++) {
			if (g_acpi_ssdt_tables[i].aml_base == NULL ||
			    g_acpi_ssdt_tables[i].aml_length == 0)
				continue;

			struct aml_parse_state state;

			memset(&state, 0, sizeof(state));
			state.aml = g_acpi_ssdt_tables[i].aml_base;
			state.aml_len = g_acpi_ssdt_tables[i].aml_length;
			state.offset = 0;
			state.parent = (uint16_t)root_idx;
			state.ssdt_index = (uint8_t)(i + 1);

			kprintf("[AML] Building namespace from SSDT[%d] (%u bytes AML)\n",
				i, (unsigned int)state.aml_len);

			while (state.offset < state.aml_len) {
				uint32_t consumed = aml_walk_terms(&state, state.aml_len);

				if (consumed == 0) {
					kprintf("[AML] Warning: parse stuck at SSDT[%d] offset %u\n",
						i, (unsigned int)state.offset);
					break;
				}
				state.offset += consumed;
			}
		}
	}

	kprintf("[AML] Namespace built: %d nodes total\n", g_ns.count);

	return 0;
}

/* ── Namespace Lookup ───────────────────────────────────────────── */

struct aml_ns_node *aml_ns_lookup(const char *path)
{
	if (!path || g_ns.count == 0)
		return NULL;

	/* Start at root or search relative to root */
	int current;

	if (path[0] == '\\') {
		/* Absolute path: start at root */
		current = 0;  /* root is always index 0 */
		path++;
	} else {
		current = 0;  /* relative from root */
	}

	/* Skip leading root prefix duplication */
	while (path[0] == '\\')
		path++;

	/* Path is empty -> return root */
	if (path[0] == '\0')
		return &g_ns.nodes[0];

	/* Walk path segments separated by '.' */
	char seg[5];
	int seg_len = 0;

	while (*path) {
		seg_len = 0;
		while (*path && *path != '.' && seg_len < 4) {
			seg[seg_len++] = *path;
			path++;
		}
		while (seg_len < 4)
			seg[seg_len++] = '_';  /* pad with underscores */
		/* Pad any remaining */
		while (*path == '.')
			path++;

		/* Find child with this name */
		struct aml_ns_node *parent_node = &g_ns.nodes[current];
		int child = parent_node->first_child;
		int found = 0;

		while (child != 0xFFFF) {
			if (memcmp(g_ns.nodes[child].name, seg, 4) == 0) {
				current = child;
				found = 1;
				break;
			}
			child = g_ns.nodes[child].next_sibling;
		}

		if (!found)
			return NULL;
	}

	return &g_ns.nodes[current];
}

int aml_ns_get_count(void)
{
	return g_ns.count;
}

struct aml_ns_node *aml_ns_get_node(int index)
{
	if (index < 0 || index >= g_ns.count)
		return NULL;
	return &g_ns.nodes[index];
}

/* ── Namespace Dump ─────────────────────────────────────────────── */

static const char *aml_ns_type_name(uint8_t type)
{
	switch (type) {
	case AML_NS_ROOT:          return "ROOT";
	case AML_NS_SCOPE:         return "SCOPE";
	case AML_NS_DEVICE:        return "DEVICE";
	case AML_NS_PROCESSOR:     return "PROC";
	case AML_NS_POWERRESOURCE: return "POWR";
	case AML_NS_THERMAL_ZONE:  return "THRM";
	case AML_NS_NAME:          return "NAME";
	case AML_NS_METHOD:        return "MTHD";
	default:                   return "????";
	}
}

void aml_dump_namespace(void)
{
	kprintf("[AML] Namespace tree (%d nodes):\n", g_ns.count);
	kprintf("  %-4s %-6s %-4s %-8s %-8s %s\n",
		"Idx", "Type", "Name", "Parent", "Child", "Src");

	for (int i = 0; i < g_ns.count; i++) {
		const struct aml_ns_node *node = &g_ns.nodes[i];
		const char *src_label = (node->from_ssdt == 0) ? "DSDT" : "SSDT ";

		kprintf("  %-4d %-6s %-4.4s %-8d %-8d %s %u\n",
			i,
			aml_ns_type_name(node->type),
			node->name,
			node->parent,
			node->first_child,
			src_label,
			(unsigned int)node->from_ssdt);
	}

	kprintf("[AML] End of namespace dump\n");
}
