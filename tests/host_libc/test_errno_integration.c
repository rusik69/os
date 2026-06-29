/*
 * test_errno_integration.c — Integration test: kernel errno.h vs system errno.h
 *
 * Verifies that every errno constant defined in the kernel's errno.h matches
 * the corresponding constant in the system's userspace <errno.h>.
 *
 * Uses two compilation units:
 *   errno_integration_data.c (compiled with KERNEL_CFLAGS) — kernel values
 *   test_errno_integration.c  (compiled with TEST_CFLAGS)  — system values
 *
 * Also tests strerror() from errno_ext.c for all kernel errno values.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

/* ===================================================================
 *  Kernel errno values (from separate compilation unit)
 * =================================================================== */
struct errno_entry {
	const char *name;
	int value;
};

extern const struct errno_entry k_errno_table[];
extern const int k_errno_table_count;

/* ===================================================================
 *  System errno values (from this compilation unit's <errno.h>)
 * =================================================================== */
static const struct errno_entry sys_errno_table[] = {
	{"EPERM",  EPERM},
	{"ENOENT", ENOENT},
	{"ESRCH",  ESRCH},
	{"EINTR",  EINTR},
	{"EIO",    EIO},
	{"ENXIO",  ENXIO},
	{"E2BIG",  E2BIG},
	{"ENOEXEC", ENOEXEC},
	{"EBADF",  EBADF},
	{"ECHILD", ECHILD},
	{"EAGAIN", EAGAIN},
	{"ENOMEM", ENOMEM},
	{"EACCES", EACCES},
	{"EFAULT", EFAULT},
	{"EBUSY",  EBUSY},
	{"EEXIST", EEXIST},
	{"EXDEV",  EXDEV},
	{"ENODEV", ENODEV},
	{"ENOTDIR", ENOTDIR},
	{"EISDIR", EISDIR},
	{"EINVAL", EINVAL},
	{"ENFILE", ENFILE},
	{"EMFILE", EMFILE},
	{"ENOTTY", ENOTTY},
	{"EFBIG",  EFBIG},
	{"ENOSPC", ENOSPC},
	{"ESPIPE", ESPIPE},
	{"EROFS",  EROFS},
	{"EMLINK", EMLINK},
	{"EPIPE",  EPIPE},
	{"ERANGE", ERANGE},
	{"EDEADLK", EDEADLK},
	{"ENAMETOOLONG", ENAMETOOLONG},
	{"ENOLCK", ENOLCK},
	{"ENOSYS", ENOSYS},
	{"ENOTEMPTY", ENOTEMPTY},
	{"ELOOP",  ELOOP},
	{"ENOMSG", ENOMSG},
	{"EIDRM",  EIDRM},
	{"EBADE",  EBADE},
	{"ENOSTR", ENOSTR},
	{"ENODATA", ENODATA},
	{"ETIME",  ETIME},
	{"ENOSR",  ENOSR},
	{"EREMOTE", EREMOTE},
	{"ENOLINK", ENOLINK},
	{"EPROTO", EPROTO},
	{"EBADMSG", EBADMSG},
	{"EOVERFLOW", EOVERFLOW},
	{"ENOTUNIQ", ENOTUNIQ},
	{"EBADFD", EBADFD},
	{"EILSEQ", EILSEQ},
	{"ERESTART", ERESTART},
	{"ESTRPIPE", ESTRPIPE},
	{"EUSERS", EUSERS},
	{"ENOTSOCK", ENOTSOCK},
	{"EDESTADDRREQ", EDESTADDRREQ},
	{"EMSGSIZE", EMSGSIZE},
	{"EPROTOTYPE", EPROTOTYPE},
	{"ENOPROTOOPT", ENOPROTOOPT},
	{"EPROTONOSUPPORT", EPROTONOSUPPORT},
	{"ESOCKTNOSUPPORT", ESOCKTNOSUPPORT},
	{"EOPNOTSUPP", EOPNOTSUPP},
	{"EPFNOSUPPORT", EPFNOSUPPORT},
	{"EAFNOSUPPORT", EAFNOSUPPORT},
	{"EADDRINUSE", EADDRINUSE},
	{"EADDRNOTAVAIL", EADDRNOTAVAIL},
	{"ENETDOWN", ENETDOWN},
	{"ENETUNREACH", ENETUNREACH},
	{"ENETRESET", ENETRESET},
	{"ECONNABORTED", ECONNABORTED},
	{"ECONNRESET", ECONNRESET},
	{"ENOBUFS", ENOBUFS},
	{"EISCONN", EISCONN},
	{"ENOTCONN", ENOTCONN},
	{"ESHUTDOWN", ESHUTDOWN},
	{"ETOOMANYREFS", ETOOMANYREFS},
	{"ETIMEDOUT", ETIMEDOUT},
	{"ECONNREFUSED", ECONNREFUSED},
	{"EHOSTDOWN", EHOSTDOWN},
	{"EHOSTUNREACH", EHOSTUNREACH},
	{"EALREADY", EALREADY},
	{"EINPROGRESS", EINPROGRESS},
	{"ESTALE", ESTALE},
	{"EUCLEAN", EUCLEAN},
	{"ENOTNAM", ENOTNAM},
	{"ENAVAIL", ENAVAIL},
	{"EISNAM", EISNAM},
	{"EREMOTEIO", EREMOTEIO},
	{"EDQUOT", EDQUOT},
	{"ENOMEDIUM", ENOMEDIUM},
	{"EMEDIUMTYPE", EMEDIUMTYPE},
	{"ECANCELED", ECANCELED},
	{"ENOKEY", ENOKEY},
	{"EKEYEXPIRED", EKEYEXPIRED},
	{"EKEYREVOKED", EKEYREVOKED},
	{"EKEYREJECTED", EKEYREJECTED},
	{"EOWNERDEAD", EOWNERDEAD},
	{"ENOTRECOVERABLE", ENOTRECOVERABLE},
	{"ERFKILL", ERFKILL},
	{"EHWPOISON", EHWPOISON},
};

static const int sys_errno_table_count = sizeof(sys_errno_table) / sizeof(sys_errno_table[0]);

/* ===================================================================
 *  Kernel strerror() (from errno_ext.c, linked in)
 * =================================================================== */
extern char *strerror(int errnum);

/* ===================================================================
 *  Stubs for kernel functions needed by errno_ext.c
 * =================================================================== */
void vga_putchar(char c)      { (void)c; }
void serial_putchar(char c)   { (void)c; }
int kprintf(const char *fmt, ...) { (void)fmt; return 0; }
int console_loglevel = 7;
int default_message_loglevel = 6;

/* ===================================================================
 *  Test harness
 * =================================================================== */
static int tests_passed = 0;
static int tests_failed = 0;
static int test_total = 0;

#define TEST(name, cond) do {                                           \
	test_total++;                                                       \
	if (!(cond)) {                                                      \
		printf("  FAIL: %s (%s)\n", name, #cond);                      \
		tests_failed++;                                                 \
	} else {                                                            \
		printf("  PASS: %s\n", name);                                   \
		tests_passed++;                                                 \
	}                                                                   \
} while (0)

#define TEST_STR(name, got, expected) do {                              \
	test_total++;                                                       \
	if (!got || strcmp(got, expected) != 0) {                           \
		printf("  FAIL: %s = \"%s\" (expected \"%s\")\n",               \
		       name, got ? got : "NULL", expected);                     \
		tests_failed++;                                                 \
	} else {                                                            \
		printf("  PASS: %s = \"%s\"\n", name, got);                     \
		tests_passed++;                                                 \
	}                                                                   \
} while (0)

/* ===================================================================
 *  Test: kernel errno values vs system errno values
 * =================================================================== */
static void test_errno_values(void)
{
	printf("--- Kernel errno values vs system errno.h ---\n");

	/* For each kernel errno constant, verify it matches the system value */
	for (int i = 0; i < k_errno_table_count; i++) {
		const char *name = k_errno_table[i].name;
		int k_val = k_errno_table[i].value;

		/* Look up the same constant in the system table */
		int found = 0;
		int s_val = 0;
		for (int j = 0; j < sys_errno_table_count; j++) {
			if (strcmp(sys_errno_table[j].name, name) == 0) {
				s_val = sys_errno_table[j].value;
				found = 1;
				break;
			}
		}

		if (!found) {
			/* Kernel-only constant (like EFSCORRUPTED — kernel-internal) */
			printf("  INFO: %s = %d (kernel only, not in userspace errno.h)\n",
			       name, k_val);
			continue;
		}

		char buf[128];
		snprintf(buf, sizeof(buf), "%s: kernel=%d system=%d", name, k_val, s_val);
		TEST(buf, k_val == s_val);
	}

	/* Check system-only constants that the kernel doesn't define */
	printf("\n--- System-only errno constants (kernel does not define) ---\n");
	for (int i = 0; i < sys_errno_table_count; i++) {
		const char *name = sys_errno_table[i].name;
		int s_val = sys_errno_table[i].value;

		int found_in_kernel = 0;
		for (int j = 0; j < k_errno_table_count; j++) {
			if (strcmp(k_errno_table[j].name, name) == 0) {
				found_in_kernel = 1;
				break;
			}
		}

		if (!found_in_kernel) {
			printf("  INFO: %s = %d (system defines, kernel does not)\n",
			       name, s_val);
		}
	}
}

/* ===================================================================
 *  Test: strerror() returns correct Linux strings for kernel errno values
 * =================================================================== */
static void test_strerror_values(void)
{
	printf("\n--- strerror() returns expected Linux strings ---\n");

	struct { int errnum; const char *expected; } str_tests[] = {
		{0,     "Success"},
		{1,     "Operation not permitted"},
		{2,     "No such file or directory"},
		{3,     "No such process"},
		{4,     "Interrupted system call"},
		{5,     "I/O error"},
		{6,     "No such device or address"},
		{7,     "Argument list too long"},
		{8,     "Exec format error"},
		{9,     "Bad file number"},
		{10,    "No child processes"},
		{11,    "Try again"},
		{12,    "Out of memory"},
		{13,    "Permission denied"},
		{14,    "Bad address"},
		{16,    "Device or resource busy"},
		{17,    "File exists"},
		{18,    "Cross-device link"},
		{19,    "No such device"},
		{20,    "Not a directory"},
		{21,    "Is a directory"},
		{22,    "Invalid argument"},
		{23,    "File table overflow"},
		{24,    "Too many open files"},
		{25,    "Not a typewriter"},
		{27,    "File too large"},
		{28,    "No space left on device"},
		{29,    "Illegal seek"},
		{30,    "Read-only file system"},
		{31,    "Too many links"},
		{32,    "Broken pipe"},
		{34,    "Math result not representable"},
		{35,    "Resource deadlock would occur"},
		{36,    "File name too long"},
		{37,    "No record locks available"},
		{38,    "Function not implemented"},
		{39,    "Directory not empty"},
		{40,    "Too many symbolic links encountered"},
		{42,    "No message of desired type"},
		{43,    "Identifier removed"},
		{52,    "Bad exchange descriptor"},
		{60,    "Device not a stream"},
		{61,    "No data available"},
		{62,    "Timer expired"},
		{63,    "Out of streams resources"},
		{66,    "Object is remote"},
		{67,    "Link has been severed"},
		{71,    "Protocol error"},
		{74,    "Bad message"},
		{75,    "Value too large for defined data type"},
		{76,    "Name not unique on network"},
		{77,    "File descriptor in bad state"},
		{84,    "Illegal byte sequence"},
		{85,    "Interrupted system call should be restarted"},
		{86,    "Streams pipe error"},
		{87,    "Too many users"},
		{88,    "Socket operation on non-socket"},
		{89,    "Destination address required"},
		{90,    "Message too long"},
		{91,    "Protocol wrong type for socket"},
		{92,    "Protocol not available"},
		{93,    "Protocol not supported"},
		{94,    "Socket type not supported"},
		{95,    "Operation not supported on transport endpoint"},
		{96,    "Protocol family not supported"},
		{97,    "Address family not supported by protocol"},
		{98,    "Address already in use"},
		{99,    "Cannot assign requested address"},
		{100,   "Network is down"},
		{101,   "Network is unreachable"},
		{102,   "Network dropped connection because of reset"},
		{103,   "Software caused connection abort"},
		{104,   "Connection reset by peer"},
		{105,   "No buffer space available"},
		{106,   "Transport endpoint is already connected"},
		{107,   "Transport endpoint is not connected"},
		{108,   "Cannot send after transport endpoint shutdown"},
		{109,   "Too many references: cannot splice"},
		{110,   "Connection timed out"},
		{111,   "Connection refused"},
		{112,   "Host is down"},
		{113,   "No route to host"},
		{114,   "Operation already in progress"},
		{115,   "Operation now in progress"},
		{116,   "Stale file handle"},
		{117,   "Structure needs cleaning"},
		{118,   "Not a XENIX named type file"},
		{119,   "No XENIX semaphores available"},
		{120,   "Is a named type file"},
		{121,   "Remote I/O error"},
		{122,   "Quota exceeded"},
		{123,   "No medium found"},
		{124,   "Wrong medium type"},
		{125,   "Operation canceled"},
		{126,   "Required key not available"},
		{127,   "Key has expired"},
		{128,   "Key has been revoked"},
		{129,   "Key was rejected by service"},
		{130,   "Owner died"},
		{131,   "State not recoverable"},
		{132,   "Operation not possible due to RF-kill"},
		{133,   "Memory page has hardware error"},
		{134,   "Filesystem corruption"},
	};

	for (size_t i = 0; i < sizeof(str_tests) / sizeof(str_tests[0]); i++) {
		char name_buf[64];
		snprintf(name_buf, sizeof(name_buf), "strerror(%d)", str_tests[i].errnum);
		char *got = strerror(str_tests[i].errnum);
		TEST_STR(name_buf, got, str_tests[i].expected);
	}
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void)
{
	printf("=== Errno Integration Test ===\n");

	test_errno_values();
	test_strerror_values();

	printf("\n============================================\n");
	printf("  Results: %d run, %d passed, %d failed\n",
	       test_total, tests_passed, tests_failed);
	printf("============================================\n");

	return tests_failed > 0 ? 1 : 0;
}
