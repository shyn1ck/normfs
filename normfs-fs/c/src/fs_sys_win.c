/*
 * The Windows bodies behind normfs/fs_sys.h. Never given to Frama-C. A
 * descriptor is a HANDLE and os_error a Win32 code, which is what
 * io::Error::from_raw_os_error reads on Windows.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>

#include "normfs/fs_sys.h"
#include "normfs/fs_dir.h"

void
normfs_fs_world_open_ok(const char *path, size_t path_len, uint64_t ino)
{
	(void)path;
	(void)path_len;
	(void)ino;
}

void
normfs_fs_world_write_ok(uint64_t ino, uint64_t n)
{
	(void)ino;
	(void)n;
}

void
normfs_fs_world_write_err(uint64_t ino)
{
	(void)ino;
}

void
normfs_fs_world_fsync_ok(uint64_t ino)
{
	(void)ino;
}

void
normfs_fs_world_fsync_err(uint64_t ino)
{
	(void)ino;
}

void
normfs_fs_world_rename_ok(const char *src, size_t src_len, const char *dst,
	size_t dst_len)
{
	(void)src;
	(void)src_len;
	(void)dst;
	(void)dst_len;
}

void
normfs_fs_world_fsync_dir_ok(const char *name, size_t name_len)
{
	(void)name;
	(void)name_len;
}

void
normfs_fs_world_fsync_dir_err(const char *name, size_t name_len)
{
	(void)name;
	(void)name_len;
}

void
normfs_fs_world_truncate_ok(uint64_t ino, uint64_t len)
{
	(void)ino;
	(void)len;
}

void
normfs_fs_world_unlink_ok(const char *path, size_t path_len)
{
	(void)path;
	(void)path_len;
}

static int
normfs_fs_sys_fail(int *os_error)
{
	DWORD e = GetLastError();

	*os_error = (e > 0u && e <= 0x7FFFFFFFu) ? (int)e : ERROR_GEN_FAILURE;
	return -1;
}

/* Paths arrive as UTF-8; the ANSI calls would read them in the local code page. */
static int
normfs_fs_sys_widen(const char *path, size_t path_len, wchar_t *out, int *os_error)
{
	int n = 0;

	if (path_len > 0u)
		n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, (int)path_len,
		    out, (int)path_len);
	if (n <= 0) {
		*os_error = ERROR_INVALID_NAME;
		return -1;
	}
	out[n] = L'\0';
	return 0;
}

static int
normfs_fs_sys_absent(DWORD e)
{
	return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
}

/* FILE_SHARE_DELETE lets a later publish rename over, or a cleanup remove, a
 * file a reader still has open, as on Unix. */
normfs_fs_fd
normfs_fs_sys_open_create(const char *path, size_t path_len, int mode,
	uint64_t *ino, int *os_error)
{
	wchar_t wide[NORMFS_FS_PATH_MAX + 1];
	BY_HANDLE_FILE_INFORMATION info;
	HANDLE h;

	*os_error = 0;
	*ino = 0u;
	if (normfs_fs_sys_widen(path, path_len, wide, os_error) != 0)
		return -1;

	h = CreateFileW(wide, GENERIC_WRITE,
	    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	    mode == NORMFS_FS_TMP_TRUNC ? CREATE_ALWAYS : CREATE_NEW,
	    FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return normfs_fs_sys_fail(os_error);
	if (!GetFileInformationByHandle(h, &info)) {
		(void)normfs_fs_sys_fail(os_error);
		(void)CloseHandle(h);
		return -1;
	}
	*ino = ((uint64_t)info.nFileIndexHigh << 32) | info.nFileIndexLow;
	if (*ino == 0u) {
		*os_error = ERROR_INVALID_DATA;
		(void)CloseHandle(h);
		return -1;
	}
	return (normfs_fs_fd)h;
}

int
normfs_fs_sys_pwritev_all(normfs_fs_fd fd, const struct normfs_fs_iov *iov,
	size_t cnt, uint64_t off, int *os_error)
{
	HANDLE h = (HANDLE)fd;
	size_t i;

	*os_error = 0;
	for (i = 0u; i < cnt; i++) {
		const uint8_t *p = iov[i].base;
		size_t left = iov[i].len;

		while (left > 0u) {
			OVERLAPPED ov;
			DWORD want = left > 0x40000000u ? 0x40000000u : (DWORD)left;
			DWORD n = 0u;

			memset(&ov, 0, sizeof(ov));
			ov.Offset = (DWORD)off;
			ov.OffsetHigh = (DWORD)(off >> 32);
			if (!WriteFile(h, p, want, &n, &ov))
				return normfs_fs_sys_fail(os_error);
			/* No progress would loop forever. */
			if (n == 0u) {
				*os_error = ERROR_WRITE_FAULT;
				return -1;
			}
			p += n;
			left -= n;
			off += n;
		}
	}
	return 0;
}

int
normfs_fs_sys_fsync(normfs_fs_fd fd, int *os_error)
{
	*os_error = 0;
	if (!FlushFileBuffers((HANDLE)fd))
		return normfs_fs_sys_fail(os_error);
	return 0;
}

int
normfs_fs_sys_close(normfs_fs_fd fd, int *os_error)
{
	*os_error = 0;
	if (!CloseHandle((HANDLE)fd))
		return normfs_fs_sys_fail(os_error);
	return 0;
}

/* A directory or a reparse point is present but not a regular file, as
 * lstat's S_ISREG would have it. */
int
normfs_fs_sys_file_len(const char *path, size_t path_len, uint64_t *len,
	int *os_error)
{
	wchar_t wide[NORMFS_FS_PATH_MAX + 1];
	WIN32_FILE_ATTRIBUTE_DATA data;

	*os_error = 0;
	*len = 0u;
	if (normfs_fs_sys_widen(path, path_len, wide, os_error) != 0)
		return -1;

	if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &data)) {
		if (normfs_fs_sys_absent(GetLastError()))
			return 0;
		return normfs_fs_sys_fail(os_error);
	}
	if (data.dwFileAttributes &
	    (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
		*os_error = ERROR_ALREADY_EXISTS;
		return -1;
	}
	*len = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
	return 1;
}

/* Plain rename refuses an existing target; POSIX rename replaces it. */
int
normfs_fs_sys_rename(const char *src, size_t src_len, const char *dst,
	size_t dst_len, int *os_error)
{
	wchar_t wsrc[NORMFS_FS_PATH_MAX + 1];
	wchar_t wdst[NORMFS_FS_PATH_MAX + 1];

	*os_error = 0;
	if (normfs_fs_sys_widen(src, src_len, wsrc, os_error) != 0 ||
	    normfs_fs_sys_widen(dst, dst_len, wdst, os_error) != 0)
		return -1;

	if (!MoveFileExW(wsrc, wdst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		return normfs_fs_sys_fail(os_error);
	return 0;
}

/* A directory cannot be flushed here without write access to it. NTFS logs
 * name changes, and the rename above is already written through. */
int
normfs_fs_sys_fsync_parent(const char *path, size_t path_len, int *os_error)
{
	(void)path;
	(void)path_len;
	*os_error = 0;
	return 0;
}

int
normfs_fs_sys_ftruncate(normfs_fs_fd fd, uint64_t len, int *os_error)
{
	FILE_END_OF_FILE_INFO info;

	*os_error = 0;
	info.EndOfFile.QuadPart = (LONGLONG)len;
	if (!SetFileInformationByHandle((HANDLE)fd, FileEndOfFileInfo, &info, sizeof(info)))
		return normfs_fs_sys_fail(os_error);
	return 0;
}

int
normfs_fs_sys_unlink(const char *path, size_t path_len, int *os_error)
{
	wchar_t wide[NORMFS_FS_PATH_MAX + 1];

	*os_error = 0;
	if (normfs_fs_sys_widen(path, path_len, wide, os_error) != 0)
		return -1;

	if (!DeleteFileW(wide)) {
		if (normfs_fs_sys_absent(GetLastError()))
			return 1;
		return normfs_fs_sys_fail(os_error);
	}
	return 0;
}

int
normfs_fs_sys_sync_dir(const char *path, size_t path_len, int *os_error)
{
	return normfs_fs_sys_fsync_parent(path, path_len, os_error);
}
