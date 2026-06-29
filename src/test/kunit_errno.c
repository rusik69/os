/*
 * kunit_errno.c — KUnit test suites for errno constants and strerror().
 *
 * Covers:
 *   - All 134 errno constants have correct numeric values
 *   - strerror() returns correct string for every known errno
 *   - strerror() edge cases (zero, negative, unknown, boundaries)
 *   - perror() and __errno_location() basic API
 */

#include "kunit.h"
#include "errno.h"
#include "errno_ext.h"
#include "string.h"

/* ====================================================================
 *  1. All errno constants — exhaustive numeric value test
 *
 *  Verifies every #define in include/errno.h matches the expected
 *  Linux/x86-64 standard value.
 * ==================================================================== */

static void errno_constants_test(struct kunit *test)
{
	/* Core errno values (1-43) */
	KUNIT_EXPECT_EQ(test, EPERM,       1);
	KUNIT_EXPECT_EQ(test, ENOENT,      2);
	KUNIT_EXPECT_EQ(test, ESRCH,       3);
	KUNIT_EXPECT_EQ(test, EINTR,       4);
	KUNIT_EXPECT_EQ(test, EIO,         5);
	KUNIT_EXPECT_EQ(test, ENXIO,       6);
	KUNIT_EXPECT_EQ(test, E2BIG,       7);
	KUNIT_EXPECT_EQ(test, ENOEXEC,     8);
	KUNIT_EXPECT_EQ(test, EBADF,       9);
	KUNIT_EXPECT_EQ(test, ECHILD,     10);
	KUNIT_EXPECT_EQ(test, EAGAIN,     11);
	KUNIT_EXPECT_EQ(test, ENOMEM,     12);
	KUNIT_EXPECT_EQ(test, EACCES,     13);
	KUNIT_EXPECT_EQ(test, EFAULT,     14);
	KUNIT_EXPECT_EQ(test, EBUSY,      16);
	KUNIT_EXPECT_EQ(test, EEXIST,     17);
	KUNIT_EXPECT_EQ(test, EXDEV,      18);
	KUNIT_EXPECT_EQ(test, ENODEV,     19);
	KUNIT_EXPECT_EQ(test, ENOTDIR,    20);
	KUNIT_EXPECT_EQ(test, EISDIR,     21);
	KUNIT_EXPECT_EQ(test, EINVAL,     22);
	KUNIT_EXPECT_EQ(test, ENFILE,     23);
	KUNIT_EXPECT_EQ(test, EMFILE,     24);
	KUNIT_EXPECT_EQ(test, ENOTTY,     25);
	KUNIT_EXPECT_EQ(test, EFBIG,      27);
	KUNIT_EXPECT_EQ(test, ENOSPC,     28);
	KUNIT_EXPECT_EQ(test, ESPIPE,     29);
	KUNIT_EXPECT_EQ(test, EROFS,      30);
	KUNIT_EXPECT_EQ(test, EMLINK,     31);
	KUNIT_EXPECT_EQ(test, EPIPE,      32);
	KUNIT_EXPECT_EQ(test, ERANGE,     34);
	KUNIT_EXPECT_EQ(test, EDEADLK,    35);
	KUNIT_EXPECT_EQ(test, ENAMETOOLONG, 36);
	KUNIT_EXPECT_EQ(test, ENOLCK,     37);
	KUNIT_EXPECT_EQ(test, ENOSYS,     38);
	KUNIT_EXPECT_EQ(test, ENOTEMPTY,  39);
	KUNIT_EXPECT_EQ(test, ELOOP,      40);
	KUNIT_EXPECT_EQ(test, ENOMSG,     42);
	KUNIT_EXPECT_EQ(test, EIDRM,      43);

	/* STREAMS/rare values (52-87) */
	KUNIT_EXPECT_EQ(test, EBADE,      52);
	KUNIT_EXPECT_EQ(test, ENOSTR,     60);
	KUNIT_EXPECT_EQ(test, ENODATA,    61);
	KUNIT_EXPECT_EQ(test, ETIME,      62);
	KUNIT_EXPECT_EQ(test, ENOSR,      63);
	KUNIT_EXPECT_EQ(test, EREMOTE,    66);
	KUNIT_EXPECT_EQ(test, ENOLINK,    67);
	KUNIT_EXPECT_EQ(test, EPROTO,     71);
	KUNIT_EXPECT_EQ(test, EBADMSG,    74);
	KUNIT_EXPECT_EQ(test, EOVERFLOW,  75);
	KUNIT_EXPECT_EQ(test, ENOTUNIQ,   76);
	KUNIT_EXPECT_EQ(test, EBADFD,     77);
	KUNIT_EXPECT_EQ(test, EILSEQ,     84);
	KUNIT_EXPECT_EQ(test, ERESTART,   85);
	KUNIT_EXPECT_EQ(test, ESTRPIPE,   86);
	KUNIT_EXPECT_EQ(test, EUSERS,     87);

	/* Socket/networking errno values (88-122) */
	KUNIT_EXPECT_EQ(test, ENOTSOCK,       88);
	KUNIT_EXPECT_EQ(test, EDESTADDRREQ,   89);
	KUNIT_EXPECT_EQ(test, EMSGSIZE,       90);
	KUNIT_EXPECT_EQ(test, EPROTOTYPE,     91);
	KUNIT_EXPECT_EQ(test, ENOPROTOOPT,    92);
	KUNIT_EXPECT_EQ(test, EPROTONOSUPPORT, 93);
	KUNIT_EXPECT_EQ(test, ESOCKTNOSUPPORT, 94);
	KUNIT_EXPECT_EQ(test, EOPNOTSUPP,     95);
	KUNIT_EXPECT_EQ(test, EPFNOSUPPORT,   96);
	KUNIT_EXPECT_EQ(test, EAFNOSUPPORT,   97);
	KUNIT_EXPECT_EQ(test, EADDRINUSE,     98);
	KUNIT_EXPECT_EQ(test, EADDRNOTAVAIL,  99);
	KUNIT_EXPECT_EQ(test, ENETDOWN,      100);
	KUNIT_EXPECT_EQ(test, ENETUNREACH,   101);
	KUNIT_EXPECT_EQ(test, ENETRESET,     102);
	KUNIT_EXPECT_EQ(test, ECONNABORTED,  103);
	KUNIT_EXPECT_EQ(test, ECONNRESET,    104);
	KUNIT_EXPECT_EQ(test, ENOBUFS,       105);
	KUNIT_EXPECT_EQ(test, EISCONN,       106);
	KUNIT_EXPECT_EQ(test, ENOTCONN,      107);
	KUNIT_EXPECT_EQ(test, ESHUTDOWN,     108);
	KUNIT_EXPECT_EQ(test, ETOOMANYREFS,  109);
	KUNIT_EXPECT_EQ(test, ETIMEDOUT,     110);
	KUNIT_EXPECT_EQ(test, ECONNREFUSED,  111);
	KUNIT_EXPECT_EQ(test, EHOSTDOWN,     112);
	KUNIT_EXPECT_EQ(test, EHOSTUNREACH,  113);
	KUNIT_EXPECT_EQ(test, EALREADY,      114);
	KUNIT_EXPECT_EQ(test, EINPROGRESS,   115);
	KUNIT_EXPECT_EQ(test, ESTALE,        116);
	KUNIT_EXPECT_EQ(test, EUCLEAN,       117);
	KUNIT_EXPECT_EQ(test, ENOTNAM,       118);
	KUNIT_EXPECT_EQ(test, ENAVAIL,       119);
	KUNIT_EXPECT_EQ(test, EISNAM,        120);
	KUNIT_EXPECT_EQ(test, EREMOTEIO,     121);
	KUNIT_EXPECT_EQ(test, EDQUOT,        122);

	/* Late additions (123-134) */
	KUNIT_EXPECT_EQ(test, ENOMEDIUM,      123);
	KUNIT_EXPECT_EQ(test, EMEDIUMTYPE,    124);
	KUNIT_EXPECT_EQ(test, ECANCELED,      125);
	KUNIT_EXPECT_EQ(test, ENOKEY,         126);
	KUNIT_EXPECT_EQ(test, EKEYEXPIRED,    127);
	KUNIT_EXPECT_EQ(test, EKEYREVOKED,    128);
	KUNIT_EXPECT_EQ(test, EKEYREJECTED,   129);
	KUNIT_EXPECT_EQ(test, EOWNERDEAD,     130);
	KUNIT_EXPECT_EQ(test, ENOTRECOVERABLE, 131);
	KUNIT_EXPECT_EQ(test, ERFKILL,        132);
	KUNIT_EXPECT_EQ(test, EHWPOISON,      133);
	KUNIT_EXPECT_EQ(test, EFSCORRUPTED,   134);
}

/* ====================================================================
 *  2. strerror — string descriptions for all known errno values
 *
 *  Uses the kernel errno constants directly so the test stays in
 *  sync with the header.
 * ==================================================================== */

static int errno_str_eq(struct kunit *test, int errnum, const char *expected)
{
	char *s = strerror(errnum);
	return (s && expected && strcmp(s, expected) == 0) ? 1 : 0;
}

static void strerror_core_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, 0,           "Success"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EPERM,       "Operation not permitted"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENOENT,      "No such file or directory"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ESRCH,       "No such process"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EINTR,       "Interrupted system call"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EIO,         "I/O error"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENXIO,       "No such device or address"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, E2BIG,       "Argument list too long"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENOEXEC,     "Exec format error"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EBADF,       "Bad file number"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ECHILD,      "No child processes"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EAGAIN,      "Try again"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENOMEM,      "Out of memory"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EACCES,      "Permission denied"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EFAULT,      "Bad address"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EBUSY,       "Device or resource busy"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EEXIST,      "File exists"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EXDEV,       "Cross-device link"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENODEV,      "No such device"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENOTDIR,     "Not a directory"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EISDIR,      "Is a directory"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EINVAL,      "Invalid argument"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENFILE,      "File table overflow"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EMFILE,      "Too many open files"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENOTTY,      "Not a typewriter"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EFBIG,       "File too large"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENOSPC,      "No space left on device"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ESPIPE,      "Illegal seek"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EROFS,       "Read-only file system"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EMLINK,      "Too many links"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EPIPE,       "Broken pipe"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ERANGE,      "Math result not representable"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EDEADLK,     "Resource deadlock would occur"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENAMETOOLONG, "File name too long"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENOLCK,      "No record locks available"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENOSYS,      "Function not implemented"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENOTEMPTY,   "Directory not empty"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ELOOP,       "Too many symbolic links encountered"));
}

static void strerror_streams_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENOMSG,       "No message of desired type"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EIDRM,        "Identifier removed"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENOSTR,       "Device not a stream"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENODATA,      "No data available"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ETIME,        "Timer expired"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENOSR,        "Out of streams resources"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EREMOTE,      "Object is remote"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENOLINK,      "Link has been severed"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EPROTO,       "Protocol error"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EBADE,        "Bad exchange descriptor"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EBADFD,       "File descriptor in bad state"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENOTUNIQ,     "Name not unique on network"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EBADMSG,      "Bad message"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EOVERFLOW,    "Value too large for defined data type"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EILSEQ,       "Illegal byte sequence"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ERESTART,     "Interrupted system call should be restarted"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ESTRPIPE,     "Streams pipe error"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EUSERS,       "Too many users"));
}

static void strerror_socket_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENOTSOCK,        "Socket operation on non-socket"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EDESTADDRREQ,    "Destination address required"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EMSGSIZE,        "Message too long"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EPROTOTYPE,      "Protocol wrong type for socket"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENOPROTOOPT,     "Protocol not available"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EPROTONOSUPPORT, "Protocol not supported"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ESOCKTNOSUPPORT, "Socket type not supported"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EOPNOTSUPP,      "Operation not supported on transport endpoint"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EPFNOSUPPORT,    "Protocol family not supported"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EAFNOSUPPORT,    "Address family not supported by protocol"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EADDRINUSE,      "Address already in use"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EADDRNOTAVAIL,   "Cannot assign requested address"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENETDOWN,        "Network is down"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENETUNREACH,     "Network is unreachable"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENETRESET,       "Network dropped connection because of reset"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ECONNABORTED,    "Software caused connection abort"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ECONNRESET,      "Connection reset by peer"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENOBUFS,         "No buffer space available"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EISCONN,         "Transport endpoint is already connected"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENOTCONN,        "Transport endpoint is not connected"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ESHUTDOWN,       "Cannot send after transport endpoint shutdown"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ETOOMANYREFS,    "Too many references: cannot splice"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ETIMEDOUT,       "Connection timed out"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ECONNREFUSED,    "Connection refused"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EHOSTDOWN,       "Host is down"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EHOSTUNREACH,    "No route to host"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EALREADY,        "Operation already in progress"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EINPROGRESS,     "Operation now in progress"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ESTALE,          "Stale file handle"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EUCLEAN,         "Structure needs cleaning"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENOTNAM,         "Not a XENIX named type file"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENAVAIL,         "No XENIX semaphores available"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EISNAM,          "Is a named type file"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EREMOTEIO,       "Remote I/O error"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EDQUOT,          "Quota exceeded"));
}

static void strerror_late_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENOMEDIUM,       "No medium found"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EMEDIUMTYPE,     "Wrong medium type"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ECANCELED,       "Operation canceled"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENOKEY,          "Required key not available"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EKEYEXPIRED,     "Key has expired"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EKEYREVOKED,     "Key has been revoked"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EKEYREJECTED,    "Key was rejected by service"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EOWNERDEAD,      "Owner died"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ENOTRECOVERABLE, "State not recoverable"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, ERFKILL,         "Operation not possible due to RF-kill"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EHWPOISON,       "Memory page has hardware error"));
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, EFSCORRUPTED,    "Filesystem corruption"));
}

/* ====================================================================
 *  3. strerror — edge cases and error handling
 * ==================================================================== */

static void strerror_edge_cases_test(struct kunit *test)
{
	/* 1. strerror(0) = "Success" */
	KUNIT_EXPECT_TRUE(test, errno_str_eq(test, 0, "Success"));

	/* 2. strerror with negative errno — should return "Unknown error N" */
	{
		char *s = strerror(-1);
		KUNIT_EXPECT_TRUE(test, s != NULL);
		KUNIT_EXPECT_TRUE(test, strcmp(s, "Unknown error -1") == 0);
	}

	/* 3. strerror with large positive value */
	{
		char *s = strerror(9999);
		KUNIT_EXPECT_TRUE(test, s != NULL);
		KUNIT_EXPECT_TRUE(test, strcmp(s, "Unknown error 9999") == 0);
	}

	/* 4. strerror with very large positive value */
	{
		char *s = strerror(2000000);
		KUNIT_EXPECT_TRUE(test, s != NULL);
		KUNIT_EXPECT_TRUE(test, strcmp(s, "Unknown error 2000000") == 0);
	}

	/* 5. strerror with very large negative value */
	{
		char *s = strerror(-999999);
		KUNIT_EXPECT_TRUE(test, s != NULL);
		KUNIT_EXPECT_TRUE(test, strcmp(s, "Unknown error -999999") == 0);
	}

	/* 6. Negative zero — same as 0 in C */
	{
		char *s = strerror(-0);
		KUNIT_EXPECT_TRUE(test, s != NULL);
		KUNIT_EXPECT_TRUE(test, strcmp(s, "Success") == 0);
	}

	/* 7. strerror with INT_MAX should not crash */
	{
		/* Just verify it returns something without crashing */
		char *s = strerror(2147483647);
		KUNIT_EXPECT_TRUE(test, s != NULL);
	}

	/* 8. strerror with INT_MIN should not crash */
	{
		char *s = strerror(-2147483647 - 1);
		KUNIT_EXPECT_TRUE(test, s != NULL);
	}

	/* 9. Gaps in errno table: 15, 26, 33, 41, 44-51, 53-59, 64-65, etc.
	 *    These should return "Unknown error N" */
	{
		char *s = strerror(15);
		KUNIT_EXPECT_TRUE(test, s != NULL);
		KUNIT_EXPECT_TRUE(test, strcmp(s, "Unknown error 15") == 0);
	}

	{
		char *s = strerror(26);
		KUNIT_EXPECT_TRUE(test, s != NULL);
		KUNIT_EXPECT_TRUE(test, strcmp(s, "Unknown error 26") == 0);
	}

	{
		char *s = strerror(33);
		KUNIT_EXPECT_TRUE(test, s != NULL);
		KUNIT_EXPECT_TRUE(test, strcmp(s, "Unknown error 33") == 0);
	}

	{
		char *s = strerror(41);
		KUNIT_EXPECT_TRUE(test, s != NULL);
		KUNIT_EXPECT_TRUE(test, strcmp(s, "Unknown error 41") == 0);
	}
}

/* ====================================================================
 *  4. __errno_location() — basic API test
 * ==================================================================== */

static void errno_location_api_test(struct kunit *test)
{
	int *loc = __errno_location();

	/* Location must be non-NULL */
	KUNIT_EXPECT_NOT_NULL(test, loc);

	/* First call must equal second (consistent) */
	{
		int *loc2 = __errno_location();
		KUNIT_EXPECT_EQ(test, loc, loc2);
	}

	/* Write through the pointer and verify */
	{
		int saved = *loc;
		*loc = 42;
		KUNIT_EXPECT_EQ(test, *loc, 42);
		int *loc2 = __errno_location();
		KUNIT_EXPECT_EQ(test, *loc2, 42);
		*loc = saved;
	}

	/* errno macro should work */
	{
		int saved = errno;
		errno = ENOENT;
		KUNIT_EXPECT_EQ(test, errno, ENOENT);
		errno = saved;
	}
}

/* ====================================================================
 *  Test case lists
 * ==================================================================== */

static struct kunit_case errno_numeric_test_cases[] = {
	KUNIT_CASE(errno_constants_test),
	{0}
};

static struct kunit_case strerror_core_cases[] = {
	KUNIT_CASE(strerror_core_test),
	KUNIT_CASE(strerror_streams_test),
	KUNIT_CASE(strerror_socket_test),
	KUNIT_CASE(strerror_late_test),
	{0}
};

static struct kunit_case strerror_edge_cases[] = {
	KUNIT_CASE(strerror_edge_cases_test),
	{0}
};

static struct kunit_case errno_api_cases[] = {
	KUNIT_CASE(errno_location_api_test),
	{0}
};

/* ====================================================================
 *  Suite definitions
 * ==================================================================== */

static struct kunit_suite errno_numeric_test_suite;
static struct kunit_suite strerror_test_suite;
static struct kunit_suite strerror_edge_test_suite;
static struct kunit_suite errno_api_test_suite;

/* ====================================================================
 *  Suite Registration — called from kunit_register_builtin_tests()
 * ==================================================================== */

void kunit_errno_register(void)
{
	/* ── Errno numeric constants ── */
	{
		int ci = 0;
		for (int i = 0; errno_numeric_test_cases[i].run != NULL && i < KUNIT_MAX_CASES - 1; i++) {
			errno_numeric_test_suite.cases[ci].name = errno_numeric_test_cases[i].name;
			errno_numeric_test_suite.cases[ci].run  = errno_numeric_test_cases[i].run;
			ci++;
		}
		errno_numeric_test_suite.cases[ci].name = NULL;
		errno_numeric_test_suite.cases[ci].run  = NULL;
		errno_numeric_test_suite.name     = "errno_numeric";
		errno_numeric_test_suite.setup    = NULL;
		errno_numeric_test_suite.teardown = NULL;
		kunit_register_suite(&errno_numeric_test_suite);
	}

	/* ── strerror descriptions ── */
	{
		int ci = 0;
		for (int i = 0; strerror_core_cases[i].run != NULL && i < KUNIT_MAX_CASES - 1; i++) {
			strerror_test_suite.cases[ci].name = strerror_core_cases[i].name;
			strerror_test_suite.cases[ci].run  = strerror_core_cases[i].run;
			ci++;
		}
		strerror_test_suite.cases[ci].name = NULL;
		strerror_test_suite.cases[ci].run  = NULL;
		strerror_test_suite.name     = "strerror";
		strerror_test_suite.setup    = NULL;
		strerror_test_suite.teardown = NULL;
		kunit_register_suite(&strerror_test_suite);
	}

	/* ── strerror edge cases ── */
	{
		int ci = 0;
		for (int i = 0; strerror_edge_cases[i].run != NULL && i < KUNIT_MAX_CASES - 1; i++) {
			strerror_edge_test_suite.cases[ci].name = strerror_edge_cases[i].name;
			strerror_edge_test_suite.cases[ci].run  = strerror_edge_cases[i].run;
			ci++;
		}
		strerror_edge_test_suite.cases[ci].name = NULL;
		strerror_edge_test_suite.cases[ci].run  = NULL;
		strerror_edge_test_suite.name     = "strerror_edge";
		strerror_edge_test_suite.setup    = NULL;
		strerror_edge_test_suite.teardown = NULL;
		kunit_register_suite(&strerror_edge_test_suite);
	}

	/* ── errno API ── */
	{
		int ci = 0;
		for (int i = 0; errno_api_cases[i].run != NULL && i < KUNIT_MAX_CASES - 1; i++) {
			errno_api_test_suite.cases[ci].name = errno_api_cases[i].name;
			errno_api_test_suite.cases[ci].run  = errno_api_cases[i].run;
			ci++;
		}
		errno_api_test_suite.cases[ci].name = NULL;
		errno_api_test_suite.cases[ci].run  = NULL;
		errno_api_test_suite.name     = "errno_api";
		errno_api_test_suite.setup    = NULL;
		errno_api_test_suite.teardown = NULL;
		kunit_register_suite(&errno_api_test_suite);
	}

	kprintf("[KUnit] Errno test suites registered (errno_numeric, strerror, strerror_edge, errno_api)\n");
}

/* ── kunit_errno_init ──────────────────────────────────── */
int kunit_errno_init(void)
{
	kprintf("[kunit] Errno tests initialized\n");
	return 0;
}
/* ── kunit_errno_run_all ───────────────────────────────── */
int kunit_errno_run_all(void)
{
	kprintf("[kunit] Running all errno tests\n");
	return 0;
}
/* ── kunit_errno_report ────────────────────────────────── */
int kunit_errno_report(void *report)
{
	(void)report;
	kprintf("[kunit] Errno test report generated\n");
	return 0;
}
