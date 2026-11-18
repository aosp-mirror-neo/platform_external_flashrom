/*
 * This file is part of the flashrom project.
 *
 * Copyright 2026 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <include/test.h>
#include "io_mock.h"
#include "tests.h"

#include "../ipc_lock.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif

#define MAX_OPEN_CALLS 8

struct file_lock_test_state {
	int open_count;
	char open_path_history[MAX_OPEN_CALLS][64];
	int open_flags_history[MAX_OPEN_CALLS];
	mode_t open_mode_history[MAX_OPEN_CALLS];
	int open_script_len;
	int open_script_ret[MAX_OPEN_CALLS];
	int open_script_errno[MAX_OPEN_CALLS];
	int last_open_flags;
	int fstat_ret;
	int fstat_errno;
	struct stat fstat_buf;
	int fchmod_count;
	mode_t last_fchmod_mode;
	int flock_count;
	int last_flock_op;
	int flock_ret;
	int flock_errno;
	int read_count;
	const char *lockfile_read_data;
	int proc_locks_opened;
	int proc_locks_closed;
	int proc_comm_opened;
	int proc_comm_closed;
	int unexpected_fopen_count;
	bool fail_proc_locks_open;
	bool fail_proc_comm_open;
	const char *proc_locks_data;
	size_t proc_locks_pos;
	const char *proc_comm_data;
	size_t proc_comm_pos;
};

static int proc_locks_sentinel;
static int proc_comm_sentinel;

static FILE *const MOCK_PROC_LOCKS_FP = (FILE *)&proc_locks_sentinel;
static FILE *const MOCK_PROC_COMM_FP = (FILE *)&proc_comm_sentinel;

int file_lock_test_teardown(void **state)
{
	(void)state;
	io_mock_register(NULL);
	return 0;
}

static int mock_lock_open(void *state, const char *pathname, int flags, mode_t mode)
{
	struct file_lock_test_state *st = state;
	int idx = st->open_count++;

	st->last_open_flags = flags;
	if (idx < MAX_OPEN_CALLS) {
		snprintf(st->open_path_history[idx], sizeof(st->open_path_history[idx]),
			 "%s", pathname ? pathname : "");
		st->open_flags_history[idx] = flags;
		st->open_mode_history[idx] = mode;
	}
	if (idx < st->open_script_len) {
		if (st->open_script_ret[idx] < 0)
			errno = st->open_script_errno[idx];
		return st->open_script_ret[idx];
	}
	return MOCK_FD;
}

static int mock_lock_fstat(void *state, int fd, struct stat *buf)
{
	struct file_lock_test_state *st = state;

	assert_int_equal(fd, MOCK_FD);
	if (st->fstat_ret != 0) {
		errno = st->fstat_errno;
		return st->fstat_ret;
	}
	*buf = st->fstat_buf;
	return 0;
}

static int mock_lock_fchmod(void *state, int fd, mode_t mode)
{
	struct file_lock_test_state *st = state;

	assert_int_equal(fd, MOCK_FD);
	st->fchmod_count++;
	st->last_fchmod_mode = mode;
	return 0;
}

static int mock_lock_flock(void *state, int fd, int operation)
{
	struct file_lock_test_state *st = state;

	assert_int_equal(fd, MOCK_FD);
	st->flock_count++;
	st->last_flock_op = operation;
	if (st->flock_ret != 0) {
		errno = st->flock_errno;
		return st->flock_ret;
	}
	return 0;
}

static int mock_lock_read(void *state, int fd, void *buf, size_t sz)
{
	struct file_lock_test_state *st = state;

	assert_int_equal(fd, MOCK_FD);
	st->read_count++;
	if (!st->lockfile_read_data)
		return 0;

	size_t len = strlen(st->lockfile_read_data);
	if (len > sz)
		len = sz;
	memcpy(buf, st->lockfile_read_data, len);
	return (int)len;
}

static FILE *mock_lock_fopen(void *state, const char *pathname, const char *mode)
{
	struct file_lock_test_state *st = state;

	assert_string_equal(mode, "r");
	if (strcmp(pathname, "/proc/locks") == 0) {
		st->proc_locks_opened++;
		if (st->fail_proc_locks_open) {
			errno = EACCES;
			return NULL;
		}
		st->proc_locks_pos = 0;
		return MOCK_PROC_LOCKS_FP;
	}
	if (strcmp(pathname, "/proc/4242/comm") == 0) {
		st->proc_comm_opened++;
		if (st->fail_proc_comm_open) {
			errno = ENOENT;
			return NULL;
		}
		st->proc_comm_pos = 0;
		return MOCK_PROC_COMM_FP;
	}

	st->unexpected_fopen_count++;
	return NULL;
}

static char *read_mock_line(const char *data, size_t *pos, char *buf, int len)
{
	if (!data || len <= 1)
		return NULL;

	size_t total = strlen(data);
	if (*pos >= total)
		return NULL;

	size_t max_copy = (size_t)(len - 1);
	size_t i = 0;
	while (i < max_copy && *pos < total) {
		char c = data[(*pos)++];
		buf[i++] = c;
		if (c == '\n')
			break;
	}
	buf[i] = '\0';
	return buf;
}

static char *mock_lock_fgets(void *state, char *buf, int len, FILE *fp)
{
	struct file_lock_test_state *st = state;

	if (fp == MOCK_PROC_LOCKS_FP)
		return read_mock_line(st->proc_locks_data, &st->proc_locks_pos, buf, len);
	if (fp == MOCK_PROC_COMM_FP)
		return read_mock_line(st->proc_comm_data, &st->proc_comm_pos, buf, len);
	return NULL;
}

static int mock_lock_fclose(void *state, FILE *fp)
{
	struct file_lock_test_state *st = state;

	if (fp == MOCK_PROC_LOCKS_FP) {
		st->proc_locks_closed++;
		return 0;
	}
	if (fp == MOCK_PROC_COMM_FP) {
		st->proc_comm_closed++;
		return 0;
	}
	return -1;
}

void file_lock_acquire_release_test_success(void **state)
{
	(void)state;

	struct file_lock_test_state test_state = {
		.fstat_ret = 0,
		.fstat_buf = {
			.st_mode = S_IFREG | 0666,
			.st_nlink = 1,
			.st_uid = geteuid(),
		},
		.flock_ret = 0,
	};
	const struct io_mock mock_io = {
		.state = &test_state,
		.iom_open = mock_lock_open,
		.iom_fstat = mock_lock_fstat,
		.iom_fchmod = mock_lock_fchmod,
		.iom_flock = mock_lock_flock,
	};

	io_mock_register(&mock_io);

	struct ipc_lock lock = LOCKFILE_INIT("firmware_utility_lock");
	assert_int_equal(acquire_lock(&lock, 0), 0);
	assert_int_equal(test_state.open_count, 1);
	assert_int_equal(test_state.last_open_flags & (O_RDWR | O_NOFOLLOW | O_CLOEXEC),
			 (O_RDWR | O_NOFOLLOW | O_CLOEXEC));
	assert_int_equal(test_state.last_open_flags & O_CREAT, 0);
	assert_int_equal(test_state.fchmod_count, 0);
	assert_int_equal(test_state.last_flock_op, LOCK_EX | LOCK_NB);
	assert_int_equal(lock.is_held, 1);

	/* Re-acquiring an already held lock returns 1 without reopening. */
	assert_int_equal(acquire_lock(&lock, 0), 1);
	assert_int_equal(test_state.open_count, 1);

	assert_int_equal(release_lock(&lock), 0);
	assert_int_equal(test_state.last_flock_op, LOCK_UN);
	assert_int_equal(lock.is_held, 0);
	assert_int_equal(lock.fd, -1);

	/* Releasing when not held returns -1. */
	assert_int_equal(release_lock(&lock), -1);
}

void file_lock_create_and_race_open_test_success(void **state)
{
	(void)state;

	const int base_flags = O_RDWR | O_NOFOLLOW | O_CLOEXEC;
	const int create_flags = base_flags | O_CREAT | O_EXCL;

	/*
	 * Case 1: Lockfile does not exist yet (ENOENT on first open) -> creates
	 * exclusively with O_CREAT | O_EXCL and mode 0666.
	 */
	struct file_lock_test_state create_state = {
		.open_script_len = 2,
		.open_script_ret = { -1, MOCK_FD },
		.open_script_errno = { ENOENT, 0 },
		.fstat_ret = 0,
		.fstat_buf = {
			.st_mode = S_IFREG | 0666,
			.st_nlink = 1,
			.st_uid = geteuid(),
		},
		.flock_ret = 0,
	};
	const struct io_mock create_mock = {
		.state = &create_state,
		.iom_open = mock_lock_open,
		.iom_fstat = mock_lock_fstat,
		.iom_fchmod = mock_lock_fchmod,
		.iom_flock = mock_lock_flock,
	};
	io_mock_register(&create_mock);

	struct ipc_lock lock1 = LOCKFILE_INIT("firmware_utility_lock");
	assert_int_equal(acquire_lock(&lock1, 0), 0);
	assert_int_equal(create_state.open_count, 2);
	assert_int_equal(create_state.open_flags_history[0] & (base_flags | O_CREAT), base_flags);
	assert_int_equal(create_state.open_flags_history[1] & create_flags, create_flags);
	assert_int_equal(create_state.open_mode_history[1], 0666);
	assert_int_equal(release_lock(&lock1), 0);

	/*
	 * Case 2: Concurrent creation race (ENOENT on first open, EEXIST on
	 * O_CREAT | O_EXCL) -> retries opening without O_CREAT.
	 */
	struct file_lock_test_state race_state = {
		.open_script_len = 3,
		.open_script_ret = { -1, -1, MOCK_FD },
		.open_script_errno = { ENOENT, EEXIST, 0 },
		.fstat_ret = 0,
		.fstat_buf = {
			.st_mode = S_IFREG | 0666,
			.st_nlink = 1,
			.st_uid = geteuid(),
		},
		.flock_ret = 0,
	};
	const struct io_mock race_mock = {
		.state = &race_state,
		.iom_open = mock_lock_open,
		.iom_fstat = mock_lock_fstat,
		.iom_fchmod = mock_lock_fchmod,
		.iom_flock = mock_lock_flock,
	};
	io_mock_register(&race_mock);

	struct ipc_lock lock2 = LOCKFILE_INIT("firmware_utility_lock");
	assert_int_equal(acquire_lock(&lock2, 0), 0);
	assert_int_equal(race_state.open_count, 3);
	assert_int_equal(race_state.open_flags_history[0] & (base_flags | O_CREAT), base_flags);
	assert_int_equal(race_state.open_flags_history[1] & create_flags, create_flags);
	assert_int_equal(race_state.open_flags_history[2] & (base_flags | O_CREAT), base_flags);
	assert_int_equal(release_lock(&lock2), 0);
}

void file_lock_fchmod_ownership_test_success(void **state)
{
	(void)state;

	const uid_t my_uid = geteuid();
	const uid_t other_uid = (my_uid == 0) ? 1000 : 0;

	/* Owned file with restrictive mode (0600) should be fchmod'd to 0666. */
	struct file_lock_test_state owned_state = {
		.fstat_ret = 0,
		.fstat_buf = {
			.st_mode = S_IFREG | 0600,
			.st_nlink = 1,
			.st_uid = my_uid,
		},
		.flock_ret = 0,
	};
	const struct io_mock owned_mock = {
		.state = &owned_state,
		.iom_open = mock_lock_open,
		.iom_fstat = mock_lock_fstat,
		.iom_fchmod = mock_lock_fchmod,
		.iom_flock = mock_lock_flock,
	};

	io_mock_register(&owned_mock);

	struct ipc_lock owned_lock = LOCKFILE_INIT("firmware_utility_lock");
	assert_int_equal(acquire_lock(&owned_lock, 0), 0);
	assert_int_equal(owned_state.fchmod_count, 1);
	assert_int_equal(owned_state.last_fchmod_mode, 0666);
	assert_int_equal(release_lock(&owned_lock), 0);

	/* Foreign-owned file with restrictive mode must NOT be fchmod'd. */
	struct file_lock_test_state foreign_state = {
		.fstat_ret = 0,
		.fstat_buf = {
			.st_mode = S_IFREG | 0600,
			.st_nlink = 1,
			.st_uid = other_uid,
		},
		.flock_ret = 0,
	};
	const struct io_mock foreign_mock = {
		.state = &foreign_state,
		.iom_open = mock_lock_open,
		.iom_fstat = mock_lock_fstat,
		.iom_fchmod = mock_lock_fchmod,
		.iom_flock = mock_lock_flock,
	};

	io_mock_register(&foreign_mock);

	struct ipc_lock foreign_lock = LOCKFILE_INIT("firmware_utility_lock");
	assert_int_equal(acquire_lock(&foreign_lock, 0), 0);
	assert_int_equal(foreign_state.fchmod_count, 0);
	assert_int_equal(release_lock(&foreign_lock), 0);
}

void file_lock_rejects_non_regular_file_test_success(void **state)
{
	(void)state;

	struct file_lock_test_state test_state = {
		.fstat_ret = 0,
		.fstat_buf = {
			.st_mode = S_IFIFO | 0666,
			.st_nlink = 1,
			.st_uid = geteuid(),
		},
		.flock_ret = 0,
	};
	const struct io_mock mock_io = {
		.state = &test_state,
		.iom_open = mock_lock_open,
		.iom_fstat = mock_lock_fstat,
		.iom_fchmod = mock_lock_fchmod,
		.iom_flock = mock_lock_flock,
	};

	io_mock_register(&mock_io);

	struct ipc_lock lock = LOCKFILE_INIT("firmware_utility_lock");
	assert_int_equal(acquire_lock(&lock, 0), -1);
#ifndef __ANDROID__
	assert_int_equal(test_state.open_count, 1);
#endif
	assert_int_equal(test_state.fchmod_count, 0);
	assert_int_equal(test_state.flock_count, 0);
	assert_int_equal(lock.is_held, 0);
	assert_int_equal(lock.fd, -1);
}

void file_lock_rejects_invalid_nlink_test_success(void **state)
{
	(void)state;

	const nlink_t bad_nlinks[] = { 0, 2 };
	for (size_t i = 0; i < sizeof(bad_nlinks) / sizeof(bad_nlinks[0]); i++) {
		struct file_lock_test_state test_state = {
			.fstat_ret = 0,
			.fstat_buf = {
				.st_mode = S_IFREG | 0666,
				.st_nlink = bad_nlinks[i],
				.st_uid = geteuid(),
			},
			.flock_ret = 0,
		};
		const struct io_mock mock_io = {
			.state = &test_state,
			.iom_open = mock_lock_open,
			.iom_fstat = mock_lock_fstat,
			.iom_fchmod = mock_lock_fchmod,
			.iom_flock = mock_lock_flock,
		};

		io_mock_register(&mock_io);

		struct ipc_lock lock = LOCKFILE_INIT("firmware_utility_lock");
		assert_int_equal(acquire_lock(&lock, 0), -1);
#ifndef __ANDROID__
		assert_int_equal(test_state.open_count, 1);
#endif
		assert_int_equal(test_state.fchmod_count, 0);
		assert_int_equal(test_state.flock_count, 0);
		assert_int_equal(lock.is_held, 0);
		assert_int_equal(lock.fd, -1);
	}
}

void file_lock_rejects_fstat_failure_test_success(void **state)
{
	(void)state;

	struct file_lock_test_state test_state = {
		.fstat_ret = -1,
		.fstat_errno = EIO,
	};
	const struct io_mock mock_io = {
		.state = &test_state,
		.iom_open = mock_lock_open,
		.iom_fstat = mock_lock_fstat,
		.iom_fchmod = mock_lock_fchmod,
		.iom_flock = mock_lock_flock,
	};

	io_mock_register(&mock_io);

	struct ipc_lock lock = LOCKFILE_INIT("firmware_utility_lock");
	assert_int_equal(acquire_lock(&lock, 0), -1);
#ifndef __ANDROID__
	assert_int_equal(test_state.open_count, 1);
#endif
	assert_int_equal(test_state.fchmod_count, 0);
	assert_int_equal(test_state.flock_count, 0);
	assert_int_equal(lock.is_held, 0);
	assert_int_equal(lock.fd, -1);
}

void file_lock_fallback_dir_test_success(void **state)
{
	(void)state;

#ifdef __ANDROID__
	skip();
#else
	struct stat dir_st = {0};
	if (lstat("/run/lock", &dir_st) != 0 || !S_ISDIR(dir_st.st_mode))
		skip();

	/*
	 * Case 1: /run/lock has no lockfile (ENOENT) and none can be created
	 * (EACCES on O_CREAT | O_EXCL) -> falls back to /tmp.
	 */
	struct file_lock_test_state fallback_state = {
		.open_script_len = 3,
		.open_script_ret = { -1, -1, MOCK_FD },
		.open_script_errno = { ENOENT, EACCES, 0 },
		.fstat_ret = 0,
		.fstat_buf = {
			.st_mode = S_IFREG | 0666,
			.st_nlink = 1,
			.st_uid = geteuid(),
		},
		.flock_ret = 0,
	};
	const struct io_mock fallback_mock = {
		.state = &fallback_state,
		.iom_open = mock_lock_open,
		.iom_fstat = mock_lock_fstat,
		.iom_fchmod = mock_lock_fchmod,
		.iom_flock = mock_lock_flock,
	};
	io_mock_register(&fallback_mock);

	struct ipc_lock lock1 = LOCKFILE_INIT("firmware_utility_lock");
	assert_int_equal(acquire_lock(&lock1, 0), 0);
	assert_int_equal(fallback_state.open_count, 3);
	assert_string_equal(fallback_state.open_path_history[0], "/run/lock/firmware_utility_lock");
	assert_string_equal(fallback_state.open_path_history[1], "/run/lock/firmware_utility_lock");
	assert_string_equal(fallback_state.open_path_history[2], "/tmp/firmware_utility_lock");
	assert_int_equal(release_lock(&lock1), 0);

	/*
	 * Case 2: Existing entry in /run/lock fails open() (e.g. ELOOP or
	 * EACCES) -> returns error without falling back to /tmp.
	 */
	struct file_lock_test_state no_fallback_state = {
		.open_script_len = 1,
		.open_script_ret = { -1 },
		.open_script_errno = { ELOOP },
	};
	const struct io_mock no_fallback_mock = {
		.state = &no_fallback_state,
		.iom_open = mock_lock_open,
		.iom_fstat = mock_lock_fstat,
		.iom_fchmod = mock_lock_fchmod,
		.iom_flock = mock_lock_flock,
	};
	io_mock_register(&no_fallback_mock);

	struct ipc_lock lock2 = LOCKFILE_INIT("firmware_utility_lock");
	assert_int_equal(acquire_lock(&lock2, 0), -1);
	assert_int_equal(no_fallback_state.open_count, 1);
	assert_string_equal(no_fallback_state.open_path_history[0], "/run/lock/firmware_utility_lock");
	assert_int_equal(lock2.fd, -1);

	/*
	 * Case 3: Concurrent creation in /run/lock (ENOENT -> EEXIST) followed
	 * by open() failure -> returns error without falling back to /tmp.
	 */
	struct file_lock_test_state race_fail_state = {
		.open_script_len = 3,
		.open_script_ret = { -1, -1, -1 },
		.open_script_errno = { ENOENT, EEXIST, EACCES },
	};
	const struct io_mock race_fail_mock = {
		.state = &race_fail_state,
		.iom_open = mock_lock_open,
		.iom_fstat = mock_lock_fstat,
		.iom_fchmod = mock_lock_fchmod,
		.iom_flock = mock_lock_flock,
	};
	io_mock_register(&race_fail_mock);

	struct ipc_lock lock3 = LOCKFILE_INIT("firmware_utility_lock");
	assert_int_equal(acquire_lock(&lock3, 0), -1);
	assert_int_equal(race_fail_state.open_count, 3);
	assert_string_equal(race_fail_state.open_path_history[2], "/run/lock/firmware_utility_lock");
	assert_int_equal(lock3.fd, -1);
#endif
}

void file_lock_rejects_invalid_args_test_success(void **state)
{
	(void)state;

	struct file_lock_test_state test_state = {0};
	const struct io_mock mock_io = {
		.state = &test_state,
		.iom_open = mock_lock_open,
	};
	io_mock_register(&mock_io);

	assert_int_equal(acquire_lock(NULL, 0), -1);
	assert_int_equal(release_lock(NULL), -1);

	struct ipc_lock null_name = LOCKFILE_INIT(NULL);
	assert_int_equal(acquire_lock(&null_name, 0), -1);

	struct ipc_lock empty_name = LOCKFILE_INIT("");
	assert_int_equal(acquire_lock(&empty_name, 0), -1);
	assert_int_equal(test_state.open_count, 0);

	/* Filename longer than PATH_MAX must be rejected rather than truncated. */
	char huge_name[PATH_MAX + 16];
	memset(huge_name, 'a', sizeof(huge_name) - 1);
	huge_name[sizeof(huge_name) - 1] = '\0';

	struct ipc_lock huge_lock = LOCKFILE_INIT(huge_name);
	assert_int_equal(acquire_lock(&huge_lock, 0), -1);
	assert_int_equal(test_state.open_count, 0);
	assert_int_equal(huge_lock.is_held, 0);
	assert_int_equal(huge_lock.fd, -1);
}

void file_lock_timeout_reports_holder_test_success(void **state)
{
	(void)state;

#ifndef __linux__
	skip();
#else
	char proc_locks_buf[1024];
	char long_line[300];
	memset(long_line, 'A', sizeof(long_line) - 2);
	long_line[sizeof(long_line) - 2] = '\n';
	long_line[sizeof(long_line) - 1] = '\0';

	int n = snprintf(proc_locks_buf, sizeof(proc_locks_buf),
			 "1: -> FLOCK  ADVISORY  WRITE 11111 00:1a:98765 0 EOF\n"
			 "2: POSIX  ADVISORY  WRITE 22222 00:1a:98765 0 EOF\n"
			 "3: FLOCK  ADVISORY  WRITE 33333 <none>:0 0 EOF\n"
			 "4: FLOCK  ADVISORY  WRITE 44444 00:1b:98765 0 EOF\n"
			 "5: FLOCK  ADVISORY  WRITE 55555 00:1a:98766 0 EOF\n"
			 "%s"
			 "6: FLOCK  ADVISORY  WRITE 99999999999999999999999 00:1a:98765 0 EOF\n"
			 "7: FLOCK  ADVISORY  WRITE 77777 -1:1a:98765 0 EOF\n"
			 "8: FLOCK  ADVISORY  WRITE 4242 00:1a:98765 0 EOF\n",
			 long_line);
	assert_true(n > 0 && (size_t)n < sizeof(proc_locks_buf));

	struct file_lock_test_state test_state = {
		.fstat_ret = 0,
		.fstat_buf = {
			.st_mode = S_IFREG | 0666,
			.st_nlink = 1,
			.st_uid = geteuid(),
			.st_dev = makedev(0x00, 0x1a),
			.st_ino = 98765,
		},
		.flock_ret = -1,
		.flock_errno = EWOULDBLOCK,
		.lockfile_read_data = "7890",
		.proc_locks_data = proc_locks_buf,
		.proc_comm_data = "test_proc\x1b[31m\n",
	};
	const struct io_mock mock_io = {
		.state = &test_state,
		.iom_open = mock_lock_open,
		.iom_fstat = mock_lock_fstat,
		.iom_fchmod = mock_lock_fchmod,
		.iom_flock = mock_lock_flock,
		.iom_read = mock_lock_read,
		.iom_fopen = mock_lock_fopen,
		.iom_fgets = mock_lock_fgets,
		.iom_fclose = mock_lock_fclose,
	};

	io_mock_register(&mock_io);

	struct ipc_lock lock = LOCKFILE_INIT("firmware_utility_lock");
	assert_int_equal(acquire_lock(&lock, 0), -1);
	assert_int_equal(lock.is_held, 0);
	assert_int_equal(lock.fd, -1);

	assert_int_equal(test_state.proc_locks_opened, 1);
	assert_int_equal(test_state.proc_locks_closed, 1);
	assert_int_equal(test_state.proc_comm_opened, 1);
	assert_int_equal(test_state.proc_comm_closed, 1);
	assert_int_equal(test_state.read_count, 0);
	assert_int_equal(test_state.unexpected_fopen_count, 0);
#endif
}

void file_lock_timeout_fallback_pid_test_success(void **state)
{
	(void)state;

	/*
	 * Case 1: /proc/locks is unreadable (e.g. blocked by SELinux on Android)
	 * -> falls back to reading the numeric PID recorded in lock->fd.
	 */
	struct file_lock_test_state fallback_state = {
		.fstat_ret = 0,
		.fstat_buf = {
			.st_mode = S_IFREG | 0666,
			.st_nlink = 1,
			.st_uid = geteuid(),
		},
		.flock_ret = -1,
		.flock_errno = EWOULDBLOCK,
		.fail_proc_locks_open = true,
		.lockfile_read_data = "7890\n",
	};
	const struct io_mock fallback_mock = {
		.state = &fallback_state,
		.iom_open = mock_lock_open,
		.iom_fstat = mock_lock_fstat,
		.iom_fchmod = mock_lock_fchmod,
		.iom_flock = mock_lock_flock,
		.iom_read = mock_lock_read,
		.iom_fopen = mock_lock_fopen,
		.iom_fgets = mock_lock_fgets,
		.iom_fclose = mock_lock_fclose,
	};
	io_mock_register(&fallback_mock);

	struct ipc_lock lock1 = LOCKFILE_INIT("firmware_utility_lock");
	assert_int_equal(acquire_lock(&lock1, 0), -1);
	assert_int_equal(lock1.fd, -1);
	assert_int_equal(fallback_state.read_count, 1);
	assert_int_equal(fallback_state.unexpected_fopen_count, 0);

#ifdef __linux__
	/*
	 * Case 2: /proc/locks omits the holder (e.g. separate PID namespace)
	 * -> falls back to reading the numeric PID recorded in lock->fd.
	 */
	struct file_lock_test_state ns_state = {
		.fstat_ret = 0,
		.fstat_buf = {
			.st_mode = S_IFREG | 0666,
			.st_nlink = 1,
			.st_uid = geteuid(),
			.st_dev = makedev(0x00, 0x1a),
			.st_ino = 98765,
		},
		.flock_ret = -1,
		.flock_errno = EWOULDBLOCK,
		.proc_locks_data = "1: FLOCK  ADVISORY  WRITE 1111 00:1a:22222 0 EOF\n",
		.lockfile_read_data = "5678",
	};
	const struct io_mock ns_mock = {
		.state = &ns_state,
		.iom_open = mock_lock_open,
		.iom_fstat = mock_lock_fstat,
		.iom_fchmod = mock_lock_fchmod,
		.iom_flock = mock_lock_flock,
		.iom_read = mock_lock_read,
		.iom_fopen = mock_lock_fopen,
		.iom_fgets = mock_lock_fgets,
		.iom_fclose = mock_lock_fclose,
	};
	io_mock_register(&ns_mock);

	struct ipc_lock lock2 = LOCKFILE_INIT("firmware_utility_lock");
	assert_int_equal(acquire_lock(&lock2, 0), -1);
	assert_int_equal(lock2.fd, -1);
	assert_int_equal(ns_state.proc_locks_opened, 1);
	assert_int_equal(ns_state.proc_locks_closed, 1);
	assert_int_equal(ns_state.read_count, 1);
	assert_int_equal(ns_state.unexpected_fopen_count, 0);
#endif

	/* Case 3: Malformed lockfile contents are rejected cleanly. */
	const char *const invalid_pids[] = {
		"",
		"0",
		"-42",
		"123abc",
		"999999999999999999999999999999",
	};
	for (size_t i = 0; i < sizeof(invalid_pids) / sizeof(invalid_pids[0]); i++) {
		fallback_state.read_count = 0;
		fallback_state.lockfile_read_data = invalid_pids[i];
		struct ipc_lock bad_lock = LOCKFILE_INIT("firmware_utility_lock");
		assert_int_equal(acquire_lock(&bad_lock, 0), -1);
		assert_int_equal(bad_lock.fd, -1);
		assert_int_equal(fallback_state.read_count, 1);
		assert_int_equal(fallback_state.unexpected_fopen_count, 0);
	}
}

void file_lock_timeout_edge_cases_test_success(void **state)
{
	(void)state;

#ifndef __linux__
	skip();
#else
	/* Case 1: /proc/locks cannot be opened and lockfile is empty. */
	struct file_lock_test_state open_fail_state = {
		.fstat_ret = 0,
		.fstat_buf = {
			.st_mode = S_IFREG | 0666,
			.st_nlink = 1,
			.st_uid = geteuid(),
			.st_dev = makedev(0x00, 0x1a),
			.st_ino = 98765,
		},
		.flock_ret = -1,
		.flock_errno = EWOULDBLOCK,
		.fail_proc_locks_open = true,
	};
	const struct io_mock open_fail_mock = {
		.state = &open_fail_state,
		.iom_open = mock_lock_open,
		.iom_fstat = mock_lock_fstat,
		.iom_fchmod = mock_lock_fchmod,
		.iom_flock = mock_lock_flock,
		.iom_read = mock_lock_read,
		.iom_fopen = mock_lock_fopen,
		.iom_fgets = mock_lock_fgets,
		.iom_fclose = mock_lock_fclose,
	};
	io_mock_register(&open_fail_mock);

	struct ipc_lock lock1 = LOCKFILE_INIT("firmware_utility_lock");
	assert_int_equal(acquire_lock(&lock1, 0), -1);
	assert_int_equal(lock1.fd, -1);
	assert_int_equal(open_fail_state.proc_locks_opened, 1);
	assert_int_equal(open_fail_state.proc_locks_closed, 0);
	assert_int_equal(open_fail_state.proc_comm_opened, 0);
	assert_int_equal(open_fail_state.read_count, 1);
	assert_int_equal(open_fail_state.unexpected_fopen_count, 0);

	/* Case 2: Non-positive PID (-1) and missing /proc/<pid>/comm. */
	struct file_lock_test_state edge_state = {
		.fstat_ret = 0,
		.fstat_buf = {
			.st_mode = S_IFREG | 0666,
			.st_nlink = 1,
			.st_uid = geteuid(),
			.st_dev = makedev(0x00, 0x1a),
			.st_ino = 98765,
		},
		.flock_ret = -1,
		.flock_errno = EWOULDBLOCK,
		.fail_proc_comm_open = true,
		.proc_locks_data =
			"1: FLOCK  ADVISORY  WRITE -1 00:1a:98765 0 EOF\n"
			"2: FLOCK  ADVISORY  WRITE 4242 00:1a:98765 0 EOF\n",
	};
	const struct io_mock edge_mock = {
		.state = &edge_state,
		.iom_open = mock_lock_open,
		.iom_fstat = mock_lock_fstat,
		.iom_fchmod = mock_lock_fchmod,
		.iom_flock = mock_lock_flock,
		.iom_read = mock_lock_read,
		.iom_fopen = mock_lock_fopen,
		.iom_fgets = mock_lock_fgets,
		.iom_fclose = mock_lock_fclose,
	};
	io_mock_register(&edge_mock);

	struct ipc_lock lock2 = LOCKFILE_INIT("firmware_utility_lock");
	assert_int_equal(acquire_lock(&lock2, 0), -1);
	assert_int_equal(lock2.fd, -1);
	assert_int_equal(edge_state.proc_locks_opened, 1);
	assert_int_equal(edge_state.proc_locks_closed, 1);
	assert_int_equal(edge_state.proc_comm_opened, 1);
	assert_int_equal(edge_state.proc_comm_closed, 0);
	assert_int_equal(edge_state.read_count, 0);
	assert_int_equal(edge_state.unexpected_fopen_count, 0);
#endif
}
