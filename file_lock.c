/* Copyright 2016, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *    * Neither the name of Google Inc. nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * file_lock.c: Implementation for a binary semaphore using a file lock.
 *
 * Warning: This relies on flock() which is known to be broken on NFS.
 *
 * The file will remain persistent once the lock has been used. Unfortunately,
 * unlinking the file can introduce a race condition so we leave the file
 * in place.
 *
 * The current process's PID will be written to the file for debug purposes.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif

#include "flash.h"
#include "ipc_lock.h"

#define SLEEP_INTERVAL_MS	50

static void msecs_to_timespec(int msecs, struct timespec *tmspec)
{
	tmspec->tv_sec = msecs / 1000;
	tmspec->tv_nsec = (msecs % 1000) * 1000 * 1000;
}

static int lock_is_held(struct ipc_lock *lock)
{
	return lock->is_held;
}

static int test_dir(const char *path)
{
	struct stat s;

	if (lstat(path, &s) < 0) {
		msg_gerr("Cannot stat %s.\n", path);
		return -1;
	}

	if (!S_ISDIR(s.st_mode)) {
		msg_gerr("%s is not a directory.\n", path);
		return -1;
	}

	return 0;
}

static int file_lock_try_open_or_create(const char *dir, struct ipc_lock *lock)
{
	char path[PATH_MAX];
	if (test_dir(dir))
		return -1;

	if (snprintf(path, sizeof(path), "%s/%s", dir, lock->filename) < 0)
		return -1;

	/* 0666: User/Group/Others: read, write. */
	mode_t permissions = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
	lock->fd = open(path, O_RDWR | O_CREAT, permissions);
	if (lock->fd < 0) {
		msg_gerr("Cannot open lockfile %s\n", path);
		return -1;
	}
	/* Ignore potential fchmod() failures since we might not be the owner. */
	if (fchmod(lock->fd, permissions) == -1)
		msg_gdbg("Cannot change the permissions of lockfile %s\n", path);

	msg_gdbg("Opened file lock \"%s\"\n", path);
	return 0;
}

static int file_lock_open_or_create(struct ipc_lock *lock)
{
	const char *dirs[] = {
#ifndef __ANDROID__
	// Default modern Linux lock path.
	"/run/lock",
	// Fallback to temporary directory.
	"/tmp",
#else
	// flashrom called as a subprocess with its own SELinux context.
	"/mnt/vendor/flashrom",
	// flashrom called from the console/shell. Comes last as a fallback.
	"/data/local/tmp",
#endif
	};

	if (file_lock_try_open_or_create(dirs[0], lock) == 0)
		return 0;

	for (size_t i = 1; i < ARRAY_SIZE(dirs); ++i) {
		msg_gwarn("Trying fallback directory: %s\n", dirs[i]);
		if (file_lock_try_open_or_create(dirs[i], lock) == 0)
			return 0;
	}

	msg_gerr("Failed to find usable directory for file lock\n");
	return -1;
}

static int file_lock_get(struct ipc_lock *lock, int timeout_msecs)
{
	int remaining_msecs;
	struct timespec sleep_interval;
	int ret;

	if (timeout_msecs == 0)
		return flock(lock->fd, LOCK_EX | LOCK_NB);

	if (timeout_msecs < 0)
		remaining_msecs = SLEEP_INTERVAL_MS;
	else
		remaining_msecs = timeout_msecs;

	while ((ret = flock(lock->fd, LOCK_EX | LOCK_NB)) != 0) {
		struct timespec rem;

		if (errno != EWOULDBLOCK) {
			msg_gerr("Error obtaining lock\n");
			return -1;
		}

		msecs_to_timespec(MIN(remaining_msecs, SLEEP_INTERVAL_MS),
				  &sleep_interval);

		while (nanosleep(&sleep_interval, &rem) != 0) {
			if (errno == EINTR) {
				sleep_interval = rem;
				continue;
			}
			msg_gerr("nanosleep() failed\n");
			return -1;
		}

		if (timeout_msecs < 0)
			continue;

		remaining_msecs -= SLEEP_INTERVAL_MS;
		if (remaining_msecs < 0)
			break;
	}

	if (ret != 0) {
		msg_gerr("Timed out waiting for file lock.\n");
		return -1;
	}

	return 0;
}

static int file_lock_write_pid(struct ipc_lock *lock)
{
	ssize_t len;
	/* PIDs are usually 5 digits, but we'll reserve enough room for
	   a value of 2^32 (10 digits) out of paranoia. */
	char pid_str[11];

	if (ftruncate(lock->fd, 0) < 0) {
		msg_gerr("Cannot truncate lockfile\n");
		return -1;
	}

	snprintf(pid_str, sizeof(pid_str), "%lu", (unsigned long)getpid());
	len = write(lock->fd, pid_str, strlen(pid_str));
	if (len < 0) {
		msg_gerr("Cannot write PID to lockfile\n");
		return -1;
	}

	return 0;
}

static bool read_lockfile_pid(int fd, long *pid_out)
{
	char buf[32] = {0};
	ssize_t n;
	char *end;
	long pid;

	if (fd < 0)
		return false;

	(void)lseek(fd, 0, SEEK_SET);
	do {
		n = read(fd, buf, sizeof(buf) - 1);
	} while (n < 0 && errno == EINTR);
	if (n <= 0)
		return false;
	buf[n] = '\0';

	if (!isdigit((unsigned char)buf[0]))
		return false;

	errno = 0;
	pid = strtol(buf, &end, 10);
	if (errno != 0 || end == buf || pid <= 0)
		return false;

	while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')
		end++;
	if (*end != '\0')
		return false;

	*pid_out = pid;
	return true;
}

#ifdef __linux__
static bool next_token(const char **cursor, const char **tok_start, size_t *tok_len)
{
	const char *p = *cursor;

	while (*p == ' ' || *p == '\t')
		p++;

	if (*p == '\0' || *p == '\n' || *p == '\r') {
		*cursor = p;
		return false;
	}

	*tok_start = p;
	while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
		p++;

	*tok_len = (size_t)(p - *tok_start);
	*cursor = p;
	return true;
}

/*
 * Parse a single line from Linux /proc/locks of the form:
 *   <id>: FLOCK  ADVISORY  WRITE <pid> <maj_hex>:<min_hex>:<ino_dec> <start> <end>
 * Blocked waiters contain "->" after "<id>:" and are ignored.
 */
static bool parse_proc_locks_line(const char *line, unsigned int target_maj,
				  unsigned int target_min, uintmax_t target_ino,
				  long *pid_out)
{
	const char *p = line;
	const char *tok;
	size_t tok_len;
	char pid_buf[32];
	char dev_ino_buf[64];
	char *end;
	long pid;
	unsigned long dev_maj;
	unsigned long dev_min;
	unsigned long long ino;

	/* Skip leading whitespace and the "<id>:" prefix. */
	while (*p == ' ' || *p == '\t')
		p++;
	if (!isdigit((unsigned char)*p))
		return false;
	while (isdigit((unsigned char)*p))
		p++;
	if (*p != ':')
		return false;
	p++;

	/* Field 1: lock type (must be "FLOCK"; ignores blocked waiters "->"). */
	if (!next_token(&p, &tok, &tok_len) ||
	    tok_len != 5 || memcmp(tok, "FLOCK", 5) != 0)
		return false;

	/* Field 2: mode (e.g. ADVISORY). */
	if (!next_token(&p, &tok, &tok_len))
		return false;

	/* Field 3: access (e.g. READ, WRITE). */
	if (!next_token(&p, &tok, &tok_len))
		return false;

	/* Field 4: PID. */
	if (!next_token(&p, &tok, &tok_len) || tok_len >= sizeof(pid_buf))
		return false;
	memcpy(pid_buf, tok, tok_len);
	pid_buf[tok_len] = '\0';

	errno = 0;
	pid = strtol(pid_buf, &end, 10);
	if (errno != 0 || end == pid_buf || *end != '\0')
		return false;

	/* Field 5: "<maj_hex>:<min_hex>:<ino_dec>". */
	if (!next_token(&p, &tok, &tok_len) || tok_len >= sizeof(dev_ino_buf))
		return false;
	memcpy(dev_ino_buf, tok, tok_len);
	dev_ino_buf[tok_len] = '\0';

	const char *s = dev_ino_buf;
	if (!isxdigit((unsigned char)*s))
		return false;
	errno = 0;
	dev_maj = strtoul(s, &end, 16);
	if (errno != 0 || end == s || *end != ':' || dev_maj > UINT_MAX)
		return false;

	s = end + 1;
	if (!isxdigit((unsigned char)*s))
		return false;
	errno = 0;
	dev_min = strtoul(s, &end, 16);
	if (errno != 0 || end == s || *end != ':' || dev_min > UINT_MAX)
		return false;

	s = end + 1;
	if (!isdigit((unsigned char)*s))
		return false;
	errno = 0;
	ino = strtoull(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0')
		return false;

	if ((unsigned int)dev_maj != target_maj ||
	    (unsigned int)dev_min != target_min ||
	    (uintmax_t)ino != target_ino)
		return false;

	*pid_out = pid;
	return true;
}

static void read_proc_comm(long pid, char *comm, size_t comm_size)
{
	char proc_path[64];
	FILE *fp;
	int ret;

	if (comm_size == 0 || comm_size > INT_MAX)
		return;
	comm[0] = '\0';

	if (pid <= 0)
		return;

	ret = snprintf(proc_path, sizeof(proc_path), "/proc/%ld/comm", pid);
	if (ret < 0 || (size_t)ret >= sizeof(proc_path))
		return;

	fp = fopen(proc_path, "r");
	if (!fp)
		return;

	if (fgets(comm, (int)comm_size, fp) != NULL) {
		size_t len = strlen(comm);
		if (len > 0 && comm[len - 1] == '\n')
			comm[--len] = '\0';
		/* Replace non-printable characters before logging. */
		for (size_t i = 0; i < len; i++) {
			if (!isprint((unsigned char)comm[i]))
				comm[i] = '?';
		}
	} else {
		comm[0] = '\0';
	}

	fclose(fp);
}

static bool report_lock_holder_from_proc_locks(int fd)
{
	struct stat st = {0};
	FILE *fp;
	char line[256];
	bool skip_long_line = false;
	bool found = false;

	if (fstat(fd, &st) < 0) {
		msg_gdbg("Could not stat lockfile to determine lock holder: %s\n",
			 strerror(errno));
		return false;
	}

	fp = fopen("/proc/locks", "r");
	if (!fp) {
		msg_gdbg("Could not open /proc/locks to determine lock holder: %s\n",
			 strerror(errno));
		return false;
	}

	unsigned int target_maj = major(st.st_dev);
	unsigned int target_min = minor(st.st_dev);
	uintmax_t target_ino = (uintmax_t)st.st_ino;

	while (fgets(line, sizeof(line), fp) != NULL) {
		size_t len = strlen(line);
		bool has_newline = (len > 0 && line[len - 1] == '\n');

		if (skip_long_line) {
			if (has_newline)
				skip_long_line = false;
			continue;
		}
		if (len == sizeof(line) - 1 && !has_newline) {
			skip_long_line = true;
			continue;
		}

		long pid = 0;
		if (!parse_proc_locks_line(line, target_maj, target_min, target_ino, &pid))
			continue;

		found = true;
		if (pid > 0) {
			char comm[64];
			read_proc_comm(pid, comm, sizeof(comm));
			if (comm[0] != '\0')
				msg_gerr("Lock is held by PID %ld (%s).\n", pid, comm);
			else
				msg_gerr("Lock is held by PID %ld.\n", pid);
		} else {
			msg_gerr("Lock is held by another process (PID unavailable).\n");
		}
	}

	fclose(fp);
	return found;
}
#else
static bool report_lock_holder_from_proc_locks(int fd)
{
	(void)fd;
	return false;
}
#endif

static void report_lock_holder(struct ipc_lock *lock)
{
	long pid = 0;

	if (report_lock_holder_from_proc_locks(lock->fd))
		return;

	/*
	 * Fall back to reading the PID written by file_lock_write_pid() when
	 * /proc/locks is unreadable (e.g., restricted by SELinux) or omits
	 * locks held outside the current PID namespace.
	 */
	if (read_lockfile_pid(lock->fd, &pid)) {
		msg_gerr("Lock is held by PID %ld (from lockfile).\n", pid);
		return;
	}

	msg_gerr("Could not determine lock holder.\n");
}

static void file_lock_release(struct ipc_lock *lock)
{
	if (flock(lock->fd, LOCK_UN) < 0)
		msg_gerr("Cannot release lock\n");

	if (close(lock->fd) < 0)
		msg_gerr("Cannot close lockfile\n");
}

/*
 * timeout <0 = no timeout (try forever)
 * timeout 0  = do not wait (return immediately)
 * timeout >0 = wait up to $timeout milliseconds
 *
 * returns 0 to indicate lock acquired
 * returns >0 to indicate lock was already held
 * returns <0 to indicate failed to acquire lock
 */
int acquire_lock(struct ipc_lock *lock, int timeout_msecs)
{
	/* check if it is already held */
	if (lock_is_held(lock))
		return 1;

	if (file_lock_open_or_create(lock))
		return -1;

	if (file_lock_get(lock, timeout_msecs)) {
		/* If locking fails, try to find out who holds the lock. */
		report_lock_holder(lock);

		lock->is_held = 0;
		close(lock->fd);
		return -1;
	} else {
		lock->is_held = 1;
	}

	/*
	 * Write PID to lockfile for debug purposes. Failure to write to
	 * the file should not be considered fatal. There might be something
	 * bad happening with the filesystem, but the lock has already been
	 * obtained and we may need our tools for diagnostics and repairs
	 * so we should continue anyway.
	 */
	file_lock_write_pid(lock);
	return 0;
}

/*
 * returns 0 if lock was released successfully
 * returns -1 if lock had not been held before the call
 */
int release_lock(struct ipc_lock *lock)
{
	if (lock_is_held(lock)) {
		file_lock_release(lock);
		lock->is_held = 0;
		return 0;
	}

	msg_ginfo("%s called but lock was not held on %s.\n",
			__func__, lock->filename);
	return -1;
}
