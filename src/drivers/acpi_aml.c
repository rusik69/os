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
#include "heap.h"
#include "aml_exec.h"

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

/* Additional opcodes used in method execution */
#define AML_RETURN_OP           0xA4    /* Return from method body */
#define AML_ONES_OP             0xFF    /* Ones constant (all bits set) */

/* AML arithmetic opcodes (ACPI v6.3, Section 19.6) */
#define AML_ADD_OP              0x72    /* Add(Operand, Operand, Target) */
#define AML_SUBTRACT_OP         0x73    /* Subtract(Operand, Operand, Target) */
#define AML_MULTIPLY_OP         0x74    /* Multiply(Operand, Operand, Target) */
#define AML_DIVIDE_OP           0x75    /* Divide(Operand, Operand, Target, Target) */
#define AML_SHIFT_LEFT_OP       0x76    /* ShiftLeft(Operand, Count, Target) */
#define AML_SHIFT_RIGHT_OP      0x77    /* ShiftRight(Operand, Count, Target) */

/* LocalX and ArgX opcodes (inside method bodies) */
#define AML_LOCAL0              0x60
#define AML_LOCAL1              0x61
#define AML_LOCAL2              0x62
#define AML_LOCAL3              0x63
#define AML_LOCAL4              0x64
#define AML_LOCAL5              0x65
#define AML_LOCAL6              0x66
#define AML_LOCAL7              0x67

#define AML_ARG0                0x68
#define AML_ARG1                0x69
#define AML_ARG2                0x6A
#define AML_ARG3                0x6B
#define AML_ARG4                0x6C
#define AML_ARG5                0x6D
#define AML_ARG6                0x6E

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

/* ═══════════════════════════════════════════════════════════════════
 * AML Method Invocation (Control Method Evaluation)
 *
 * Implements a minimal AML bytecode interpreter that can evaluate
 * ACPI control methods. Supports:
 *   - Local0-Local7 and Arg0-Arg6 variable access
 *   - Store (0x14) to locals and args
 *   - Return (0xA4) with integer, string, buffer, or package values
 *   - Constants: Zero, One, Ones, Byte, Word, DWord, QWord, String
 *   - Name (0x08) for creating named integer/string/buffer/package objects
 *
 * Reference: ACPI v6.3, Sections 19-20
 */

/* ── Object Lifecycle Helpers ───────────────────────────────────── */

static struct aml_object *aml_alloc_object(void)
{
	struct aml_object *obj;

	obj = (struct aml_object *)kmalloc(sizeof(struct aml_object));
	if (!obj)
		return NULL;

	memset(obj, 0, sizeof(struct aml_object));
	obj->type = AML_OBJ_UNDEFINED;
	obj->from_heap = 1;
	return obj;
}

void aml_free_object(struct aml_object *obj)
{
	if (!obj)
		return;

	/* Free owned resources based on type */
	switch (obj->type) {
	case AML_OBJ_STRING:
		if (obj->value.string.ptr && obj->from_heap)
			kfree(obj->value.string.ptr);
		break;
	case AML_OBJ_BUFFER:
		if (obj->value.buffer.data && obj->from_heap)
			kfree(obj->value.buffer.data);
		break;
	case AML_OBJ_PACKAGE:
		if (obj->value.package.elements && obj->from_heap) {
			for (uint32_t i = 0; i < obj->value.package.count; i++)
				aml_free_object(&obj->value.package.elements[i]);
			kfree(obj->value.package.elements);
		}
		break;
	default:
		break;
	}

	if (obj->from_heap)
		kfree(obj);
}

struct aml_object *aml_create_integer(uint64_t value)
{
	struct aml_object *obj = aml_alloc_object();

	if (!obj)
		return NULL;
	obj->type = AML_OBJ_INTEGER;
	obj->value.integer = value;
	return obj;
}

struct aml_object *aml_create_string(const char *str)
{
	struct aml_object *obj;
	size_t slen;

	if (!str)
		return NULL;

	obj = aml_alloc_object();
	if (!obj)
		return NULL;

	slen = strlen(str);
	obj->type = AML_OBJ_STRING;
	obj->value.string.ptr = (char *)kmalloc(slen + 1);
	if (!obj->value.string.ptr) {
		kfree(obj);
		return NULL;
	}
	memcpy(obj->value.string.ptr, str, slen + 1);
	obj->value.string.len = (uint32_t)slen;
	return obj;
}

struct aml_object *aml_create_buffer(const uint8_t *data, uint32_t length)
{
	struct aml_object *obj;

	obj = aml_alloc_object();
	if (!obj)
		return NULL;

	obj->type = AML_OBJ_BUFFER;
	if (length > 0 && data) {
		obj->value.buffer.data = (uint8_t *)kmalloc(length);
		if (!obj->value.buffer.data) {
			kfree(obj);
			return NULL;
		}
		memcpy(obj->value.buffer.data, data, length);
		obj->value.buffer.length = length;
	}
	return obj;
}

struct aml_object *aml_create_reference(int node_index)
{
	struct aml_object *obj = aml_alloc_object();

	if (!obj)
		return NULL;
	obj->type = AML_OBJ_REFERENCE;
	obj->value.ref.node_index = node_index;
	return obj;
}

/* Forward declaration for aml_clone_object (defined later in this file
 * in the "Object Copy / Clone" section).  Used by aml_create_package
 * and the AML bytecode parsing helpers. */
static struct aml_object *aml_clone_object(const struct aml_object *src);

struct aml_object *aml_create_package(const struct aml_object *elements,
				      uint32_t count)
{
	struct aml_object *obj;

	if (count > 0 && !elements)
		return NULL;

	obj = aml_alloc_object();
	if (!obj)
		return NULL;

	obj->type = AML_OBJ_PACKAGE;
	obj->value.package.count = count;

	if (count == 0) {
		obj->value.package.elements = NULL;
		return obj;
	}

	obj->value.package.elements = (struct aml_object *)
		kmalloc(count * sizeof(struct aml_object));
	if (!obj->value.package.elements) {
		kfree(obj);
		return NULL;
	}

	memset(obj->value.package.elements, 0,
	       count * sizeof(struct aml_object));

	/* Deep-copy each element */
	for (uint32_t i = 0; i < count; i++) {
		struct aml_object *clone = aml_clone_object(&elements[i]);

		if (!clone) {
			/* Partial cleanup: free elements [0..i-1] */
			for (uint32_t j = 0; j < i; j++)
				aml_free_object(&obj->value.package.elements[j]);
			kfree(obj->value.package.elements);
			kfree(obj);
			return NULL;
		}
		memcpy(&obj->value.package.elements[i], clone,
		       sizeof(struct aml_object));
		kfree(clone);
	}

	return obj;
}

/* ── Execution Context ──────────────────────────────────────────── */

struct aml_exec_context {
	const uint8_t *aml;           /* Current AML bytecode */
	uint32_t       aml_len;       /* Total AML length */
	uint32_t       offset;        /* Current execution offset */

	/* Local variables (Local0-Local7) */
	struct aml_object *locals[AML_MAX_LOCALS];
	/* Method arguments (Arg0-Arg6) */
	struct aml_object *args[AML_MAX_ARGS];

	/* Return value and execution state */
	struct aml_object  own_return;    /* Stack-allocated return buffer */
	struct aml_object *return_value;  /* Points to return value */
	int  has_returned;                /* Method has executed Return */
	int  error;                       /* Execution error occurred */
};

/* ── Method Header Parser ───────────────────────────────────────── */

/*
 * Skip past the method definition header to reach the TermList.
 * Method encoding: MethodOp PkgLength NameString MethodFlags TermList
 *
 * Returns the byte offset of the TermList start, or 0 on error.
 * If num_args_out is non-NULL, stores the argument count from MethodFlags.
 */
static uint32_t aml_skip_method_header(const uint8_t *aml, uint32_t aml_len,
				       uint8_t *num_args_out)
{
	uint32_t o = 0;
	uint32_t pkg_bytes;
	uint32_t pkg_len;

	if (!aml || aml_len < 6)
		return 0;  /* Minimum: Op(1) + PkgLen(1) + NameSeg(4) = 6 */

	if (aml[o] != AML_METHOD_OP)
		return 0;
	o++;  /* Skip MethodOp */

	/* Decode PkgLength */
	pkg_len = aml_decode_pkg_length(aml, aml_len, o, &pkg_bytes);
	if (pkg_len == 0 || pkg_len < 5)
		return 0;  /* PkgLen must cover NameSeg(4) + Flags(1) minimum */
	if (o + pkg_bytes > aml_len)
		return 0;
	o += pkg_bytes;

	/* Skip NameString */
	{
		uint32_t name_bytes = aml_skip_name_string(aml, aml_len, o);

		if (name_bytes == 0)
			return 0;
		o += name_bytes;
	}

	/* Read MethodFlags */
	if (o >= aml_len)
		return 0;

	if (num_args_out)
		*num_args_out = aml[o] & 0x07;  /* Bits[2:0] = ArgCount */
	o++;  /* Skip MethodFlags */

	/* o now points to the start of the TermList */
	return o;
}

/* ── AML Byte Decoding Helpers ──────────────────────────────────── */

/*
 * Read a little-endian unsigned value from the AML byte stream.
 * Returns 0 on bounds error (offset too large).
 */
static uint64_t aml_read_le64(const uint8_t *data, uint32_t max_len,
			      uint32_t offset, int bytes)
{
	uint64_t val = 0;

	if (offset + (uint32_t)bytes > max_len)
		return 0;

	for (int i = 0; i < bytes && i < 8; i++)
		val |= (uint64_t)data[offset + i] << (i * 8);

	return val;
}

/* ── Object Copy / Clone ────────────────────────────────────────── */

/*
 * Parse a BufferOp (0x11) from AML bytecode and return a heap-allocated
 * aml_object of type AML_OBJ_BUFFER.
 *
 * Encoding: BufferOp PkgLength BufferSize ByteList
 *   - BufferSize is a ByteConst/WordConst/DWordConst that declares the
 *     intended size of the buffer.  The actual ByteList data follows.
 *
 * On success, advances ctx->offset past the entire BufferOp and returns
 * the new object (caller owns it).  On error returns NULL.
 */
static struct aml_object *aml_parse_buffer_from_aml(
	struct aml_exec_context *ctx)
{
	uint32_t o = ctx->offset;
	uint32_t buf_bytes;
	uint32_t pkg_len;
	uint32_t data_offset;
	uint32_t size_field_bytes = 0;
	uint32_t declared_size = 0;
	struct aml_object *obj = NULL;

	if (o + 2 > ctx->aml_len)
		return NULL;

	if (ctx->aml[o] != AML_BUFFER_OP)
		return NULL;

	/* Decode PkgLength */
	pkg_len = aml_decode_pkg_length(ctx->aml, ctx->aml_len,
					o + 1, &buf_bytes);
	if (pkg_len == 0 || pkg_len < 2)
		return NULL;

	data_offset = o + 1 + buf_bytes;
	if (data_offset >= ctx->aml_len)
		return NULL;

	/* Parse BufferSize prefix */
	if (ctx->aml[data_offset] == AML_BYTE_PREFIX &&
	    data_offset + 2 <= ctx->aml_len) {
		declared_size = ctx->aml[data_offset + 1];
		size_field_bytes = 2;
	} else if (ctx->aml[data_offset] == AML_WORD_PREFIX &&
		   data_offset + 3 <= ctx->aml_len) {
		declared_size = (uint32_t)ctx->aml[data_offset + 1] |
			((uint32_t)ctx->aml[data_offset + 2] << 8);
		size_field_bytes = 3;
	} else if (ctx->aml[data_offset] == AML_DWORD_PREFIX &&
		   data_offset + 5 <= ctx->aml_len) {
		declared_size = (uint32_t)ctx->aml[data_offset + 1] |
			((uint32_t)ctx->aml[data_offset + 2] << 8) |
			((uint32_t)ctx->aml[data_offset + 3] << 16) |
			((uint32_t)ctx->aml[data_offset + 4] << 24);
		size_field_bytes = 5;
	} else {
		/* Assume raw size byte (no prefix — rare but valid) */
		declared_size = ctx->aml[data_offset];
		size_field_bytes = 1;
	}

	if (declared_size == 0) {
		/* Zero-length buffer is valid */
		obj = aml_create_buffer(NULL, 0);
		if (obj)
			ctx->offset = o + 1 + buf_bytes + pkg_len;
		return obj;
	}

	/* Check that declared_size fits within the PkgLength */
	if (data_offset + size_field_bytes + declared_size >
	    o + 1 + buf_bytes + pkg_len)
		return NULL;

	/* Read the raw byte data */
	obj = aml_create_buffer(&ctx->aml[data_offset + size_field_bytes],
				declared_size);
	if (obj)
		ctx->offset = o + 1 + buf_bytes + pkg_len;

	return obj;
}

/*
 * Parse a PackageOp (0x12) or VarPackageOp (0x13) from AML bytecode
 * and return a heap-allocated aml_object of type AML_OBJ_PACKAGE.
 *
 * Encoding: PackageOp PkgLength NumElements PackageElementList
 *   - NumElements is a ByteData containing the element count.
 *   - PackageElementList is a list of DataRefObject entries.
 *
 * For VarPackageOp the NumElements is computed from remaining bytes
 * once the fixed-length header is consumed, but we use the declared
 * NumElements from the byte stream.
 *
 * On success, advances ctx->offset past the entire PackageOp and
 * returns the new object (caller owns it).  On error returns NULL.
 */
static struct aml_object *aml_parse_package_from_aml(
	struct aml_exec_context *ctx)
{
	uint32_t o = ctx->offset;
	uint32_t pkg_bytes;
	uint32_t pkg_len;
	int is_var;
	uint32_t num_elements;
	uint32_t data_offset;
	uint32_t element_end;
	struct aml_object *obj = NULL;
	struct aml_object *elements = NULL;
	struct aml_object *saved_return;
	int saved_has_returned;
	int saved_error;

	if (o + 2 > ctx->aml_len)
		return NULL;

	if (ctx->aml[o] != AML_PACKAGE_OP &&
	    ctx->aml[o] != AML_VAR_PACKAGE_OP)
		return NULL;

	is_var = (ctx->aml[o] == AML_VAR_PACKAGE_OP);

	/* Decode PkgLength */
	pkg_len = aml_decode_pkg_length(ctx->aml, ctx->aml_len,
					o + 1, &pkg_bytes);
	if (pkg_len == 0 || pkg_len < 2)
		return NULL;

	data_offset = o + 1 + pkg_bytes;
	if (data_offset >= ctx->aml_len)
		return NULL;

	/* Read NumElements (ByteData) */
	uint8_t num_elems_byte = ctx->aml[data_offset];
	num_elements = (uint32_t)num_elems_byte;
	data_offset++;

	if (is_var && num_elements == 0)
		num_elements = 0; /* VarPackage with zero elements */

	element_end = o + 1 + pkg_bytes + pkg_len;
	if (element_end > ctx->aml_len)
		element_end = ctx->aml_len;

	if (num_elements == 0) {
		obj = aml_create_package(NULL, 0);
		if (obj)
			ctx->offset = element_end;
		return obj;
	}

	/* Allocate temporary array for element parsing */
	elements = (struct aml_object *)
		kmalloc(num_elements * sizeof(struct aml_object));
	if (!elements)
		return NULL;
	memset(elements, 0, num_elements * sizeof(struct aml_object));

	/* Save and reset the execution context so our element parsing
	 * doesn't clobber the parent context's state */
	saved_return = ctx->return_value;
	saved_has_returned = ctx->has_returned;
	saved_error = ctx->error;
	ctx->return_value = NULL;
	ctx->has_returned = 0;
	ctx->error = 0;

	/* Parse each element from the AML byte stream */
	uint32_t parse_ok = 1;

	for (uint32_t i = 0; i < num_elements && data_offset < element_end; i++) {
		struct aml_object *elem = NULL;
		uint8_t op = ctx->aml[data_offset];

		switch (op) {
		case AML_ZERO_OP:
			data_offset++;
			elem = aml_create_integer(0);
			break;
		case AML_ONE_OP:
			data_offset++;
			elem = aml_create_integer(1);
			break;
		case AML_ONES_OP:
			data_offset++;
			elem = aml_create_integer((uint64_t)-1);
			break;
		case AML_BYTE_PREFIX:
			if (data_offset + 2 <= ctx->aml_len) {
				data_offset++;
				elem = aml_create_integer(ctx->aml[data_offset]);
				data_offset++;
			}
			break;
		case AML_WORD_PREFIX:
			if (data_offset + 3 <= ctx->aml_len) {
				data_offset++;
				elem = aml_create_integer(
					aml_read_le64(ctx->aml, ctx->aml_len,
						      data_offset, 2));
				data_offset += 2;
			}
			break;
		case AML_DWORD_PREFIX:
			if (data_offset + 5 <= ctx->aml_len) {
				data_offset++;
				elem = aml_create_integer(
					aml_read_le64(ctx->aml, ctx->aml_len,
						      data_offset, 4));
				data_offset += 4;
			}
			break;
		case AML_QWORD_PREFIX:
			if (data_offset + 9 <= ctx->aml_len) {
				data_offset++;
				elem = aml_create_integer(
					aml_read_le64(ctx->aml, ctx->aml_len,
						      data_offset, 8));
				data_offset += 8;
			}
			break;
		case AML_STRING_PREFIX: {
			uint32_t start = data_offset + 1;
			uint32_t end = start;

			while (end < ctx->aml_len && ctx->aml[end] != 0)
				end++;
			if (end < ctx->aml_len) {
				char *s = (char *)kmalloc(end - start + 1);

				if (s) {
					memcpy(s, &ctx->aml[start], end - start);
					s[end - start] = '\0';
					elem = aml_create_string(s);
					kfree(s);
				}
				data_offset = end + 1;
			}
			break;
		}
		case AML_BUFFER_OP:
			ctx->offset = data_offset;
			elem = aml_parse_buffer_from_aml(ctx);
			if (elem)
				data_offset = ctx->offset;
			break;
		case AML_PACKAGE_OP:
		case AML_VAR_PACKAGE_OP:
			ctx->offset = data_offset;
			elem = aml_parse_package_from_aml(ctx);
			if (elem)
				data_offset = ctx->offset;
			break;
		default:
			/* Skip unknown element by advancing past it */
			data_offset++;
			break;
		}

		if (elem) {
			memcpy(&elements[i], elem, sizeof(struct aml_object));
			kfree(elem);
		} else {
			parse_ok = 0;
			break;
		}
	}

	/* Restore execution context */
	ctx->return_value = saved_return;
	ctx->has_returned = saved_has_returned;
	ctx->error = saved_error;

	if (parse_ok) {
		obj = aml_create_package(elements, num_elements);
		ctx->offset = element_end;
	}

	/* Free temporary element array */
	for (uint32_t i = 0; i < num_elements; i++)
		aml_free_object(&elements[i]);
	kfree(elements);

	return obj;
}

/*
 * Deep-copy an AML object.  The caller owns the returned object.
 * Returns NULL on allocation failure.
 */
static struct aml_object *aml_clone_object(const struct aml_object *src)
{
	struct aml_object *dst;

	if (!src)
		return NULL;

	dst = aml_alloc_object();
	if (!dst)
		return NULL;

	dst->type = src->type;

	switch (src->type) {
	case AML_OBJ_INTEGER:
		dst->value.integer = src->value.integer;
		break;

	case AML_OBJ_STRING:
		if (src->value.string.ptr) {
			dst->value.string.ptr = (char *)kmalloc(
				src->value.string.len + 1);
			if (!dst->value.string.ptr) {
				kfree(dst);
				return NULL;
			}
			memcpy(dst->value.string.ptr, src->value.string.ptr,
			       src->value.string.len + 1);
			dst->value.string.len = src->value.string.len;
		}
		break;

	case AML_OBJ_BUFFER:
		if (src->value.buffer.length > 0 && src->value.buffer.data) {
			dst->value.buffer.data = (uint8_t *)kmalloc(
				src->value.buffer.length);
			if (!dst->value.buffer.data) {
				kfree(dst);
				return NULL;
			}
			memcpy(dst->value.buffer.data,
			       src->value.buffer.data,
			       src->value.buffer.length);
			dst->value.buffer.length = src->value.buffer.length;
		}
		break;

	case AML_OBJ_PACKAGE:
		if (src->value.package.count > 0 &&
		    src->value.package.elements) {
			uint32_t count = src->value.package.count;

			dst->value.package.elements =
				(struct aml_object *)kmalloc(
					count * sizeof(struct aml_object));
			if (!dst->value.package.elements) {
				kfree(dst);
				return NULL;
			}
			memset(dst->value.package.elements, 0,
			       count * sizeof(struct aml_object));
			dst->value.package.count = count;

			for (uint32_t i = 0; i < count; i++) {
				struct aml_object *elem;

				elem = aml_clone_object(
					&src->value.package.elements[i]);
				if (!elem) {
					for (uint32_t j = 0; j < i; j++)
						aml_free_object(
							&dst->value.package.
							elements[j]);
					kfree(dst->value.package.elements);
					kfree(dst);
					return NULL;
				}
				memcpy(&dst->value.package.elements[i],
				       elem, sizeof(struct aml_object));
				kfree(elem);
			}
		}
		break;

	default:
		break;
	}

	return dst;
}

/* ── Store Implementation ───────────────────────────────────────── */

/*
 * Store a value to a target.
 * In AML, Store(Source, Target) copies the value to the target location.
 * Encoding: StoreOp(0x14) Operand SuperName
 *
 * The caller has already consumed the StoreOp byte; ctx->offset points
 * to the Operand.  Returns 0 on success, -1 on error.
 */
static int aml_exec_store(struct aml_exec_context *ctx)
{
	struct aml_object operand_buf;    /* Stack buffer for operand */
	struct aml_object *operand = NULL;
	int ret = -1;

	memset(&operand_buf, 0, sizeof(operand_buf));

	/* Parse the Operand (source value) */
	{
		uint8_t op = (ctx->offset < ctx->aml_len)
			     ? ctx->aml[ctx->offset] : 0;

		switch (op) {
		case AML_ZERO_OP:
			ctx->offset++;
			operand = &operand_buf;
			operand->type = AML_OBJ_INTEGER;
			operand->value.integer = 0;
			break;

		case AML_ONE_OP:
			ctx->offset++;
			operand = &operand_buf;
			operand->type = AML_OBJ_INTEGER;
			operand->value.integer = 1;
			break;

		case AML_ONES_OP:
			ctx->offset++;
			operand = &operand_buf;
			operand->type = AML_OBJ_INTEGER;
			operand->value.integer = (uint64_t)-1;
			break;

		case AML_BYTE_PREFIX:
			if (ctx->offset + 2 > ctx->aml_len)
				goto done;
			ctx->offset++;
			operand = &operand_buf;
			operand->type = AML_OBJ_INTEGER;
			operand->value.integer = ctx->aml[ctx->offset];
			ctx->offset++;
			break;

		case AML_WORD_PREFIX:
			if (ctx->offset + 3 > ctx->aml_len)
				goto done;
			ctx->offset++;
			operand = &operand_buf;
			operand->type = AML_OBJ_INTEGER;
			operand->value.integer = aml_read_le64(
				ctx->aml, ctx->aml_len, ctx->offset, 2);
			ctx->offset += 2;
			break;

		case AML_DWORD_PREFIX:
			if (ctx->offset + 5 > ctx->aml_len)
				goto done;
			ctx->offset++;
			operand = &operand_buf;
			operand->type = AML_OBJ_INTEGER;
			operand->value.integer = aml_read_le64(
				ctx->aml, ctx->aml_len, ctx->offset, 4);
			ctx->offset += 4;
			break;

		case AML_QWORD_PREFIX:
			if (ctx->offset + 9 > ctx->aml_len)
				goto done;
			ctx->offset++;
			operand = &operand_buf;
			operand->type = AML_OBJ_INTEGER;
			operand->value.integer = aml_read_le64(
				ctx->aml, ctx->aml_len, ctx->offset, 8);
			ctx->offset += 8;
			break;

		case AML_STRING_PREFIX: {
			uint32_t end;

			ctx->offset++;
			end = ctx->offset;
			while (end < ctx->aml_len && ctx->aml[end] != 0)
				end++;
			if (end >= ctx->aml_len)
				goto done;

			operand = &operand_buf;
			operand->type = AML_OBJ_STRING;
			operand->value.string.len = end - ctx->offset;
			operand->from_heap = 0;
			ctx->offset = end + 1;
			break;
		}

		case AML_LOCAL0: case AML_LOCAL1: case AML_LOCAL2:
		case AML_LOCAL3: case AML_LOCAL4: case AML_LOCAL5:
		case AML_LOCAL6: case AML_LOCAL7: {
			int idx = op - AML_LOCAL0;

			ctx->offset++;
			if (ctx->locals[idx]) {
				struct aml_object *clone;

				clone = aml_clone_object(ctx->locals[idx]);
				if (!clone)
					goto done;
				memcpy(&operand_buf, clone,
				       sizeof(operand_buf));
				kfree(clone);
			} else {
				operand = &operand_buf;
				operand->type = AML_OBJ_INTEGER;
				operand->value.integer = 0;
			}
			operand = &operand_buf;
			break;
		}

		case AML_ARG0: case AML_ARG1: case AML_ARG2:
		case AML_ARG3: case AML_ARG4: case AML_ARG5:
		case AML_ARG6: {
			int idx = op - AML_ARG0;

			ctx->offset++;
			if (ctx->args[idx]) {
				struct aml_object *clone;

				clone = aml_clone_object(ctx->args[idx]);
				if (!clone)
					goto done;
				memcpy(&operand_buf, clone,
				       sizeof(operand_buf));
				kfree(clone);
			} else {
				operand = &operand_buf;
				operand->type = AML_OBJ_INTEGER;
				operand->value.integer = 0;
			}
			operand = &operand_buf;
			break;
		}

		/* ── BufferOp: create buffer from AML bytecode ── */
		case AML_BUFFER_OP: {
			struct aml_object *buf_obj;

			buf_obj = aml_parse_buffer_from_aml(ctx);
			if (!buf_obj)
				goto done;
			operand = buf_obj;
			break;
		}

		/* ── PackageOp: create package from AML bytecode ── */
		case AML_PACKAGE_OP:
		case AML_VAR_PACKAGE_OP: {
			struct aml_object *pkg_obj;

			pkg_obj = aml_parse_package_from_aml(ctx);
			if (!pkg_obj)
				goto done;
			operand = pkg_obj;
			break;
		}

		default:
			kprintf("[AML] Store: unsupported operand "
				"opcode 0x%02x at offset %u\n",
				op, (unsigned int)ctx->offset);
			goto done;
		}
	}

	/* Parse the Target (destination: where to store) */
	{
		uint8_t op = (ctx->offset < ctx->aml_len)
			     ? ctx->aml[ctx->offset] : 0;

		switch (op) {
		case AML_LOCAL0: case AML_LOCAL1: case AML_LOCAL2:
		case AML_LOCAL3: case AML_LOCAL4: case AML_LOCAL5:
		case AML_LOCAL6: case AML_LOCAL7: {
			int idx = op - AML_LOCAL0;

			ctx->offset++;

			if (ctx->locals[idx]) {
				aml_free_object(ctx->locals[idx]);
				ctx->locals[idx] = NULL;
			}
			if (operand) {
				struct aml_object *clone;

				clone = aml_clone_object(operand);
				if (clone)
					ctx->locals[idx] = clone;
			}
			ret = 0;
			break;
		}

		case AML_ARG0: case AML_ARG1: case AML_ARG2:
		case AML_ARG3: case AML_ARG4: case AML_ARG5:
		case AML_ARG6: {
			int idx = op - AML_ARG0;

			ctx->offset++;
			if (ctx->args[idx]) {
				aml_free_object(ctx->args[idx]);
				ctx->args[idx] = NULL;
			}
			if (operand) {
				struct aml_object *clone;

				clone = aml_clone_object(operand);
				if (clone)
					ctx->args[idx] = clone;
			}
			ret = 0;
			break;
		}

		default:
			kprintf("[AML] Store: unsupported target "
				"opcode 0x%02x at offset %u\n",
				op, (unsigned int)ctx->offset);
			goto done;
		}
	}

done:
	/* Free heap-allocated operands (Buffer/Package objects) */
	if (operand && operand != &operand_buf)
		aml_free_object(operand);
	return ret;
}

/* ── Return Implementation ──────────────────────────────────────── */

/*
 * Execute a Return term.
 * Encoding: ReturnOp(0xA4) TermArg
 * Returns 0 on success, -1 on error.
 */
static int aml_exec_return(struct aml_exec_context *ctx)
{
	uint8_t op;

	if (ctx->has_returned)
		return 0;  /* Already returned, ignore subsequent */

	if (ctx->offset >= ctx->aml_len)
		return 0;

	op = ctx->aml[ctx->offset];

	/* Initialize return value buffer */
	ctx->return_value = &ctx->own_return;
	memset(ctx->return_value, 0, sizeof(*ctx->return_value));

	/* Parse the return value operand */
	switch (op) {
	case AML_ZERO_OP:
		ctx->offset++;
		ctx->return_value->type = AML_OBJ_INTEGER;
		ctx->return_value->value.integer = 0;
		break;

	case AML_ONE_OP:
		ctx->offset++;
		ctx->return_value->type = AML_OBJ_INTEGER;
		ctx->return_value->value.integer = 1;
		break;

	case AML_ONES_OP:
		ctx->offset++;
		ctx->return_value->type = AML_OBJ_INTEGER;
		ctx->return_value->value.integer = (uint64_t)-1;
		break;

	case AML_BYTE_PREFIX:
		if (ctx->offset + 2 > ctx->aml_len)
			return -1;
		ctx->offset++;
		ctx->return_value->type = AML_OBJ_INTEGER;
		ctx->return_value->value.integer = ctx->aml[ctx->offset];
		ctx->offset++;
		break;

	case AML_WORD_PREFIX:
		if (ctx->offset + 3 > ctx->aml_len)
			return -1;
		ctx->offset++;
		ctx->return_value->type = AML_OBJ_INTEGER;
		ctx->return_value->value.integer = aml_read_le64(
			ctx->aml, ctx->aml_len, ctx->offset, 2);
		ctx->offset += 2;
		break;

	case AML_DWORD_PREFIX:
		if (ctx->offset + 5 > ctx->aml_len)
			return -1;
		ctx->offset++;
		ctx->return_value->type = AML_OBJ_INTEGER;
		ctx->return_value->value.integer = aml_read_le64(
			ctx->aml, ctx->aml_len, ctx->offset, 4);
		ctx->offset += 4;
		break;

	case AML_QWORD_PREFIX:
		if (ctx->offset + 9 > ctx->aml_len)
			return -1;
		ctx->offset++;
		ctx->return_value->type = AML_OBJ_INTEGER;
		ctx->return_value->value.integer = aml_read_le64(
			ctx->aml, ctx->aml_len, ctx->offset, 8);
		ctx->offset += 8;
		break;

	case AML_STRING_PREFIX: {
		uint32_t start;

		ctx->offset++;
		start = ctx->offset;
		while (ctx->offset < ctx->aml_len &&
		       ctx->aml[ctx->offset] != 0)
			ctx->offset++;
		if (ctx->offset >= ctx->aml_len)
			return -1;

		ctx->return_value->type = AML_OBJ_STRING;
		ctx->return_value->value.string.len =
			ctx->offset - start;
		ctx->return_value->value.string.ptr =
			(char *)kmalloc(ctx->return_value->value.string.len + 1);
		if (!ctx->return_value->value.string.ptr)
			return -1;
		memcpy(ctx->return_value->value.string.ptr,
		       &ctx->aml[start],
		       ctx->return_value->value.string.len);
		ctx->return_value->value.string.ptr[
			ctx->return_value->value.string.len] = '\0';
		ctx->return_value->from_heap = 1;
		ctx->offset++;  /* skip null terminator */
		break;
	}

	case AML_LOCAL0: case AML_LOCAL1: case AML_LOCAL2:
	case AML_LOCAL3: case AML_LOCAL4: case AML_LOCAL5:
	case AML_LOCAL6: case AML_LOCAL7: {
		int idx = op - AML_LOCAL0;

		ctx->offset++;
		if (ctx->locals[idx]) {
			struct aml_object *clone;

			clone = aml_clone_object(ctx->locals[idx]);
			if (clone) {
				memcpy(ctx->return_value, clone,
				       sizeof(*clone));
				kfree(clone);
			}
		} else {
			ctx->return_value->type = AML_OBJ_INTEGER;
			ctx->return_value->value.integer = 0;
		}
		break;
	}

	case AML_ARG0: case AML_ARG1: case AML_ARG2:
	case AML_ARG3: case AML_ARG4: case AML_ARG5:
	case AML_ARG6: {
		int idx = op - AML_ARG0;

		ctx->offset++;
		if (ctx->args[idx]) {
			struct aml_object *clone;

			clone = aml_clone_object(ctx->args[idx]);
			if (clone) {
				memcpy(ctx->return_value, clone,
				       sizeof(*clone));
				kfree(clone);
			}
		} else {
			ctx->return_value->type = AML_OBJ_INTEGER;
			ctx->return_value->value.integer = 0;
		}
		break;
	}

	/* ── BufferOp: return buffer from AML bytecode ── */
	case AML_BUFFER_OP: {
		struct aml_object *buf_obj;

		buf_obj = aml_parse_buffer_from_aml(ctx);
		if (buf_obj) {
			memcpy(ctx->return_value, buf_obj,
			       sizeof(*buf_obj));
			kfree(buf_obj);
		} else {
			ctx->return_value->type = AML_OBJ_INTEGER;
			ctx->return_value->value.integer = 0;
		}
		break;
	}

	/* ── PackageOp: return package from AML bytecode ── */
	case AML_PACKAGE_OP:
	case AML_VAR_PACKAGE_OP: {
		struct aml_object *pkg_obj;

		pkg_obj = aml_parse_package_from_aml(ctx);
		if (pkg_obj) {
			memcpy(ctx->return_value, pkg_obj,
			       sizeof(*pkg_obj));
			kfree(pkg_obj);
		} else {
			ctx->return_value->type = AML_OBJ_INTEGER;
			ctx->return_value->value.integer = 0;
		}
		break;
	}

	default:
		kprintf("[AML] Return: unsupported opcode 0x%02x "
			"at offset %u\n",
			op, (unsigned int)ctx->offset);
		ctx->return_value->type = AML_OBJ_INTEGER;
		ctx->return_value->value.integer = 0;
		break;
	}

	ctx->has_returned = 1;
	return 0;
}

/* ── Arithmetic Execution Helpers ──────────────────────────────── */

/*
 * Read an integer operand from the AML byte stream at ctx->offset.
 * Advances ctx->offset past the operand encoding bytes.
 *
 * Accepts:
 *   - Integer constants: ZeroOp, OneOp, OnesOp, BytePrefix, WordPrefix,
 *     DWordPrefix, QWordPrefix
 *   - LocalX / ArgX references (resolved to their current integer value;
 *     0 if uninitialized or non-integer)
 *
 * On error, sets ctx->error and returns 0.
 */
static uint64_t aml_read_arith_operand(struct aml_exec_context *ctx)
{
	uint8_t op;

	if (ctx->offset >= ctx->aml_len) {
		ctx->error = 1;
		return 0;
	}

	op = ctx->aml[ctx->offset];
	ctx->offset++;

	switch (op) {
	case AML_ZERO_OP:
		return 0;

	case AML_ONE_OP:
		return 1;

	case AML_ONES_OP:
		return (uint64_t)-1;

	case AML_BYTE_PREFIX:
		if (ctx->offset + 1 > ctx->aml_len)
			goto error;
		{
			uint64_t v = ctx->aml[ctx->offset];

			ctx->offset++;
			return v;
		}

	case AML_WORD_PREFIX:
		if (ctx->offset + 2 > ctx->aml_len)
			goto error;
		{
			uint64_t v = aml_read_le64(ctx->aml, ctx->aml_len,
						   ctx->offset, 2);

			ctx->offset += 2;
			return v;
		}

	case AML_DWORD_PREFIX:
		if (ctx->offset + 4 > ctx->aml_len)
			goto error;
		{
			uint64_t v = aml_read_le64(ctx->aml, ctx->aml_len,
						   ctx->offset, 4);

			ctx->offset += 4;
			return v;
		}

	case AML_QWORD_PREFIX:
		if (ctx->offset + 8 > ctx->aml_len)
			goto error;
		{
			uint64_t v = aml_read_le64(ctx->aml, ctx->aml_len,
						   ctx->offset, 8);

			ctx->offset += 8;
			return v;
		}

	case AML_LOCAL0: case AML_LOCAL1: case AML_LOCAL2:
	case AML_LOCAL3: case AML_LOCAL4: case AML_LOCAL5:
	case AML_LOCAL6: case AML_LOCAL7:
	{
		int idx = op - AML_LOCAL0;

		if (ctx->locals[idx] &&
		    ctx->locals[idx]->type == AML_OBJ_INTEGER)
			return ctx->locals[idx]->value.integer;
		return 0;
	}

	case AML_ARG0: case AML_ARG1: case AML_ARG2:
	case AML_ARG3: case AML_ARG4: case AML_ARG5:
	case AML_ARG6:
	{
		int idx = op - AML_ARG0;

		if (ctx->args[idx] &&
		    ctx->args[idx]->type == AML_OBJ_INTEGER)
			return ctx->args[idx]->value.integer;
		return 0;
	}

	default:
		kprintf("[AML] Arithmetic: unsupported operand "
			"0x%02x at offset %u\n",
			op, (unsigned int)(ctx->offset - 1));
		ctx->error = 1;
		return 0;
	}

error:
	ctx->error = 1;
	return 0;
}

/*
 * Write a 64-bit integer result to a target location (LocalX or ArgX).
 * Advances ctx->offset past the target encoding.
 * Returns 0 on success, -1 on error (sets ctx->error).
 */
static int aml_write_arith_result(struct aml_exec_context *ctx,
				  uint64_t value)
{
	uint8_t op;

	if (ctx->offset >= ctx->aml_len) {
		ctx->error = 1;
		return -1;
	}

	op = ctx->aml[ctx->offset];
	ctx->offset++;

	switch (op) {
	case AML_LOCAL0: case AML_LOCAL1: case AML_LOCAL2:
	case AML_LOCAL3: case AML_LOCAL4: case AML_LOCAL5:
	case AML_LOCAL6: case AML_LOCAL7:
	{
		int idx = op - AML_LOCAL0;

		if (ctx->locals[idx]) {
			aml_free_object(ctx->locals[idx]);
			ctx->locals[idx] = NULL;
		}
		ctx->locals[idx] = aml_create_integer(value);
		return 0;
	}

	case AML_ARG0: case AML_ARG1: case AML_ARG2:
	case AML_ARG3: case AML_ARG4: case AML_ARG5:
	case AML_ARG6:
	{
		int idx = op - AML_ARG0;

		if (ctx->args[idx]) {
			aml_free_object(ctx->args[idx]);
			ctx->args[idx] = NULL;
		}
		ctx->args[idx] = aml_create_integer(value);
		return 0;
	}

	default:
		kprintf("[AML] Arithmetic: unsupported target "
			"0x%02x at offset %u\n",
			op, (unsigned int)(ctx->offset - 1));
		ctx->error = 1;
		return -1;
	}
}

/*
 * Execute an AML arithmetic operation.
 *
 * Three-operand forms (Add, Subtract, Multiply, ShiftLeft, ShiftRight):
 *   Opcode(1) Operand1 Operand2 Target
 *
 * Four-operand form (Divide):
 *   DivideOp(1) Dividend Divisor Remainder Quotient
 *
 * Returns 0 on success, -1 on error.
 */
static int aml_exec_arith(struct aml_exec_context *ctx, uint8_t opcode)
{
	uint64_t operand1;
	uint64_t operand2;
	uint64_t result = 0;

	/* Read the first operand */
	operand1 = aml_read_arith_operand(ctx);
	if (ctx->error)
		return -1;

	/* Read the second operand */
	operand2 = aml_read_arith_operand(ctx);
	if (ctx->error)
		return -1;

	switch (opcode) {
	case AML_ADD_OP:
		result = operand1 + operand2;
		break;

	case AML_SUBTRACT_OP:
		result = operand1 - operand2;
		break;

	case AML_MULTIPLY_OP:
		result = operand1 * operand2;
		break;

	case AML_DIVIDE_OP:
	{
		/*
		 * Divide has a 4-operand form:
		 *   Remainder = Dividend % Divisor
		 *   Quotient  = Dividend / Divisor
		 * Dividend=operand1, Divisor=operand2,
		 * then Remainder target, then Quotient target.
		 */
		if (operand2 == 0) {
			kprintf("[AML] Divide by zero\n");
			ctx->error = 1;
			return -1;
		}
		/* Write remainder */
		if (aml_write_arith_result(ctx,
		    operand1 % operand2) < 0)
			return -1;
		/* Write quotient */
		if (aml_write_arith_result(ctx,
		    operand1 / operand2) < 0)
			return -1;
		return 0;
	}

	case AML_SHIFT_LEFT_OP:
		result = operand1 << (operand2 & 0x3F);
		break;

	case AML_SHIFT_RIGHT_OP:
		result = operand1 >> (operand2 & 0x3F);
		break;

	default:
		kprintf("[AML] Unknown arithmetic opcode 0x%02x\n", opcode);
		ctx->error = 1;
		return -1;
	}

	/* Write result to target */
	if (aml_write_arith_result(ctx, result) < 0)
		return -1;

	return 0;
}

/* ── Term Executor ──────────────────────────────────────────────── */

/*
 * Execute a single AML term within a method body.
 * Returns the number of AML bytes consumed, or 0 on error.
 * This is the core of the AML interpreter loop.
 */
static uint32_t aml_exec_one_term(struct aml_exec_context *ctx)
{
	uint32_t o = ctx->offset;
	uint8_t opcode;

	if (o >= ctx->aml_len)
		return 0;

	opcode = ctx->aml[o];

	switch (opcode) {
	/* ── StoreOp (0x14 = MethodOp in namespace, StoreOp in body) ─ */
	case AML_METHOD_OP:
		ctx->offset++;
		if (aml_exec_store(ctx) < 0) {
			kprintf("[AML] Store failed at offset %u\n",
				(unsigned int)(o));
			ctx->error = 1;
		}
		return ctx->offset - o;

	/* ── ReturnOp (0xA4) ─────────────────────────────────────── */
	case AML_RETURN_OP:
		ctx->offset++;
		if (aml_exec_return(ctx) < 0) {
			kprintf("[AML] Return failed at offset %u\n",
				(unsigned int)(o));
			ctx->error = 1;
		}
		return ctx->offset - o;

	/* ── Constants ──────────────────────────────────────────── */
	case AML_ZERO_OP:
	case AML_ONE_OP:
	case AML_ONES_OP:
		ctx->offset++;
		return ctx->offset - o;

	case AML_BYTE_PREFIX:
		ctx->offset += (o + 2 <= ctx->aml_len) ? 2 : 1;
		return ctx->offset - o;

	case AML_WORD_PREFIX:
		ctx->offset += (o + 3 <= ctx->aml_len) ? 3 : 1;
		return ctx->offset - o;

	case AML_DWORD_PREFIX:
		ctx->offset += (o + 5 <= ctx->aml_len) ? 5 : 1;
		return ctx->offset - o;

	case AML_QWORD_PREFIX:
		ctx->offset += (o + 9 <= ctx->aml_len) ? 9 : 1;
		return ctx->offset - o;

	case AML_STRING_PREFIX: {
		uint32_t end = o + 1;

		while (end < ctx->aml_len && ctx->aml[end] != 0)
			end++;
		if (end < ctx->aml_len)
			end++;
		ctx->offset = end;
		return ctx->offset - o;
	}

	/* ── LocalX / ArgX References ───────────────────────────── */
	case AML_LOCAL0: case AML_LOCAL1: case AML_LOCAL2:
	case AML_LOCAL3: case AML_LOCAL4: case AML_LOCAL5:
	case AML_LOCAL6: case AML_LOCAL7:
	case AML_ARG0:   case AML_ARG1:   case AML_ARG2:
	case AML_ARG3:   case AML_ARG4:   case AML_ARG5:
	case AML_ARG6:
		ctx->offset++;
		return ctx->offset - o;

	/* ── Buffer / Package ───────────────────────────────────── */
	case AML_BUFFER_OP:
	case AML_PACKAGE_OP:
	case AML_VAR_PACKAGE_OP: {
		uint32_t pkg_bytes;
		uint32_t pkg_len;

		pkg_len = aml_decode_pkg_length(ctx->aml, ctx->aml_len,
						o + 1, &pkg_bytes);
		if (pkg_len == 0)
			return 1;
		ctx->offset = o + 1 + pkg_bytes + pkg_len;
		if (ctx->offset > ctx->aml_len)
			ctx->offset = ctx->aml_len;
		return ctx->offset - o;
	}

	/* ── Minimally skip over NameOp (0x08) ──────────────────── */
	case AML_NAME_OP: {
		uint32_t start_ofs = ctx->offset;
		uint32_t name_bytes;

		ctx->offset++;  /* skip NameOp */
		name_bytes = aml_skip_name_string(ctx->aml, ctx->aml_len,
						   ctx->offset);
		if (name_bytes == 0)
			return 1;
		ctx->offset += name_bytes;

		/* Skip the DataRefObject that follows the name */
		if (ctx->offset < ctx->aml_len) {
			uint8_t d_op = ctx->aml[ctx->offset];

			switch (d_op) {
			case AML_ZERO_OP:
			case AML_ONE_OP:
			case AML_ONES_OP:
				ctx->offset++;
				break;
			case AML_BYTE_PREFIX:
				ctx->offset += (ctx->offset + 2 <=
						ctx->aml_len) ? 2 : 1;
				break;
			case AML_WORD_PREFIX:
				ctx->offset += (ctx->offset + 3 <=
						ctx->aml_len) ? 3 : 1;
				break;
			case AML_DWORD_PREFIX:
				ctx->offset += (ctx->offset + 5 <=
						ctx->aml_len) ? 5 : 1;
				break;
			case AML_QWORD_PREFIX:
				ctx->offset += (ctx->offset + 9 <=
						ctx->aml_len) ? 9 : 1;
				break;
			case AML_STRING_PREFIX: {
				uint32_t end2 = ctx->offset + 1;

				while (end2 < ctx->aml_len &&
				       ctx->aml[end2] != 0)
					end2++;
				ctx->offset = (end2 < ctx->aml_len) ?
					      end2 + 1 : end2;
				break;
			}
			case AML_BUFFER_OP: {
				uint32_t buf_bytes, buf_len;

				buf_len = aml_decode_pkg_length(
					ctx->aml, ctx->aml_len,
					ctx->offset + 1, &buf_bytes);
				ctx->offset += (buf_len > 0) ?
					(1 + buf_bytes + buf_len) : 2;
				break;
			}
			case AML_PACKAGE_OP: {
				uint32_t pkg_bytes2, pkg_len2;

				pkg_len2 = aml_decode_pkg_length(
					ctx->aml, ctx->aml_len,
					ctx->offset + 1, &pkg_bytes2);
				ctx->offset += (pkg_len2 > 0) ?
					(1 + pkg_bytes2 + pkg_len2) : 2;
				break;
			}
			default:
				ctx->offset++;  /* Unknown data type, skip 1 */
				break;
			}
		}
		return ctx->offset - start_ofs;
	}

	/* ── Extended opcode prefix (0x5B) ──────────────────────── */
	case AML_EXT_OP_PREFIX:
		/* Skip ext prefix + ext opcode = 2 bytes */
		if (o + 2 > ctx->aml_len) {
			ctx->offset = ctx->aml_len;
			return 1;
		}
		ctx->offset = o + 2;
		return 2;

	/* ── Default: Unknown opcode ────────────────────────────── */
	default:
		/* AML arithmetic operations (0x72-0x77) */
		if (opcode >= AML_ADD_OP && opcode <= AML_SHIFT_RIGHT_OP) {
			ctx->offset++;
			if (aml_exec_arith(ctx, opcode) < 0) {
				kprintf("[AML] Arithmetic opcode 0x%02x "
					"failed at offset %u\n",
					opcode, (unsigned int)(o));
				ctx->error = 1;
			}
			return ctx->offset - o;
		}

		/* Many AML comparison opcodes (0x70-0x7F).
		 * Best-effort skip: opcode + 3 bytes for operands. */
		if (opcode >= 0x70 && opcode <= 0x7F) {
			ctx->offset = o + 4;
			return 4;
		}

		/* Logical comparison opcodes */
		if (opcode == 0x90 || opcode == 0x91 || opcode == 0x92 ||
		    opcode == 0x93 || opcode == 0x94) {
			ctx->offset = o + 3;
			return 3;
		}

		/* For anything unknown, advance 1 byte to avoid loop */
		ctx->offset = o + 1;
		kprintf("[AML] Skipping unknown opcode 0x%02x at offset %u\n",
			opcode, (unsigned int)o);
		return 1;
	}
}

/* ── Method Evaluator ───────────────────────────────────────────── */

/*
 * Evaluate a method's AML TermList.
 *
 * @param ctx  Initialized execution context.
 * @return 0 on success, -1 on error.
 */
static int aml_exec_method_body(struct aml_exec_context *ctx)
{
	while (ctx->offset < ctx->aml_len &&
	       !ctx->has_returned && !ctx->error) {
		uint32_t consumed = aml_exec_one_term(ctx);

		if (consumed == 0) {
			kprintf("[AML] Term execution stuck at offset %u\n",
				(unsigned int)ctx->offset);
			ctx->error = 1;
			break;
		}
	}

	return ctx->error ? -1 : 0;
}

/* ── Cleanup Context ────────────────────────────────────────────── */

static void aml_exec_context_cleanup(struct aml_exec_context *ctx)
{
	if (!ctx)
		return;

	for (int i = 0; i < AML_MAX_LOCALS; i++) {
		if (ctx->locals[i]) {
			aml_free_object(ctx->locals[i]);
			ctx->locals[i] = NULL;
		}
	}

	for (int i = 0; i < AML_MAX_ARGS; i++) {
		if (ctx->args[i]) {
			aml_free_object(ctx->args[i]);
			ctx->args[i] = NULL;
		}
	}

	/* Clean up return value heap allocations */
	if (ctx->return_value == &ctx->own_return &&
	    ctx->return_value->from_heap) {
		if (ctx->return_value->type == AML_OBJ_STRING &&
		    ctx->return_value->value.string.ptr) {
			kfree(ctx->return_value->value.string.ptr);
			ctx->return_value->value.string.ptr = NULL;
		} else if (ctx->return_value->type == AML_OBJ_BUFFER &&
			   ctx->return_value->value.buffer.data) {
			kfree(ctx->return_value->value.buffer.data);
			ctx->return_value->value.buffer.data = NULL;
		} else if (ctx->return_value->type == AML_OBJ_PACKAGE &&
			   ctx->return_value->value.package.elements) {
			for (uint32_t i = 0;
			     i < ctx->return_value->value.package.count; i++)
				aml_free_object(
					&ctx->return_value->value.package.
					elements[i]);
			kfree(ctx->return_value->value.package.elements);
			ctx->return_value->value.package.elements = NULL;
		}
	}
}

/* ── Public API: Evaluate a Control Method ───────────────────────── */

struct aml_object *aml_evaluate_method(const char *path,
				       struct aml_object *args[],
				       int num_args)
{
	struct aml_ns_node *node;
	struct aml_exec_context ctx;
	struct aml_object *result = NULL;
	uint32_t termlist_offset;
	uint8_t method_arg_count = 0;

	/* Look up the method in the namespace */
	node = aml_ns_lookup(path);
	if (!node) {
		kprintf("[AML] Method not found in namespace: %s\n",
			path ? path : "(null)");
		return NULL;
	}

	if (node->type != AML_NS_METHOD) {
		kprintf("[AML] '%s' is not a method (type %u)\n",
			path ? path : "(null)",
			(unsigned int)node->type);
		return NULL;
	}

	if (!node->aml_start || node->aml_length == 0) {
		kprintf("[AML] Method '%s' has no AML bytecode\n",
			path ? path : "(null)");
		return NULL;
	}

	/* Initialize execution context */
	memset(&ctx, 0, sizeof(ctx));
	ctx.aml = node->aml_start;
	ctx.aml_len = node->aml_length;
	ctx.return_value = NULL;
	ctx.has_returned = 0;
	ctx.error = 0;

	/* Set up method arguments */
	if (num_args > AML_MAX_ARGS)
		num_args = AML_MAX_ARGS;

	for (int i = 0; i < num_args; i++) {
		if (args && args[i]) {
			ctx.args[i] = aml_clone_object(args[i]);
		}
	}

	/* Skip past the method header to the TermList */
	termlist_offset = aml_skip_method_header(ctx.aml, ctx.aml_len,
						 &method_arg_count);
	if (termlist_offset == 0) {
		kprintf("[AML] Failed to parse method header for '%s'\n",
			path ? path : "(null)");
		goto done;
	}

	/* Validate argument count */
	if (num_args > (int)method_arg_count) {
		kprintf("[AML] Method '%s' takes %u args, got %d "
			"(continuing anyway)\n",
			path ? path : "(null)",
			(unsigned int)method_arg_count, num_args);
	}

	kprintf("[AML] Evaluating method '%s' (%u AML bytes, %u args)\n",
		path ? path : "(null)",
		(unsigned int)ctx.aml_len,
		(unsigned int)method_arg_count);

	/* Execute the method body */
	ctx.offset = termlist_offset;
	if (aml_exec_method_body(&ctx) < 0) {
		kprintf("[AML] Method '%s' execution failed\n",
			path ? path : "(null)");
		goto done;
	}

	/* Extract the return value */
	if (ctx.has_returned && ctx.return_value) {
		result = aml_clone_object(ctx.return_value);
	} else {
		result = aml_create_integer(0);
	}

done:
	aml_exec_context_cleanup(&ctx);
	return result;
}
