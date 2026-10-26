/*
 * The Windows bodies behind normfs/disk_monitor_sys.h; never given to Frama-C.
 * os_error is a Win32 code, which is what io::Error::from_raw_os_error reads
 * on Windows.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "normfs/disk_monitor.h"
#include "normfs/disk_monitor_sys.h"

/* FindFirstFileW hands back the first entry with the handle. */
struct normfs_disk_sys_dir {
	HANDLE find;
	WIN32_FIND_DATAW entry;
	int have;
};

static int
normfs_disk_sys_fail(int *os_error)
{
	DWORD e = GetLastError();

	*os_error = (e > 0u && e <= 0x7FFFFFFFu) ? (int)e : ERROR_GEN_FAILURE;
	return -1;
}

static int
normfs_disk_sys_absent(DWORD e)
{
	return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ||
	    e == ERROR_DIRECTORY;
}

/* Paths arrive as UTF-8; the ANSI calls would read them in the local code page. */
static int
normfs_disk_sys_widen(const char *path, size_t path_len, wchar_t *out)
{
	int n = 0;

	if (path_len > 0u)
		n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, (int)path_len,
		    out, (int)path_len);
	if (n <= 0)
		return -1;
	out[n] = L'\0';
	return n;
}

static int
normfs_disk_sys_dots(const wchar_t *name)
{
	return name[0] == L'.' &&
	    (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'));
}

void *
normfs_disk_sys_dir_open(const char *path, size_t path_len, int *os_error)
{
	wchar_t wide[NORMFS_DISK_PATH_MAX + 3];
	struct normfs_disk_sys_dir *d;
	int n;

	*os_error = 0;
	n = normfs_disk_sys_widen(path, path_len, wide);
	if (n < 0) {
		*os_error = ERROR_INVALID_NAME;
		return NULL;
	}
	if (wide[n - 1] != L'\\' && wide[n - 1] != L'/')
		wide[n++] = L'\\';
	wide[n++] = L'*';
	wide[n] = L'\0';

	d = malloc(sizeof(*d));
	if (d == NULL) {
		*os_error = ERROR_NOT_ENOUGH_MEMORY;
		return NULL;
	}
	d->find = FindFirstFileW(wide, &d->entry);
	if (d->find == INVALID_HANDLE_VALUE) {
		DWORD e = GetLastError();

		free(d);
		if (!normfs_disk_sys_absent(e))
			(void)normfs_disk_sys_fail(os_error);
		return NULL;
	}
	d->have = 1;
	return d;
}

/* The find data already carries what lstat would: a reparse point, Windows'
 * symlink, is OTHER, and nothing can vanish between listing and stat. */
int
normfs_disk_sys_dir_next(void *handle, char *name, size_t name_cap,
    size_t *name_len, int *kind, uint64_t *size, int *os_error)
{
	struct normfs_disk_sys_dir *d = handle;
	const WIN32_FIND_DATAW *e = &d->entry;
	int n;

	*os_error = 0;
	*kind = NORMFS_DISK_SYS_OTHER;
	*size = 0u;
	*name_len = 0u;
	name[0] = '\0';

	for (;;) {
		if (!d->have) {
			if (!FindNextFileW(d->find, &d->entry)) {
				if (GetLastError() == ERROR_NO_MORE_FILES)
					return 0;
				return normfs_disk_sys_fail(os_error);
			}
		}
		d->have = 0;
		if (!normfs_disk_sys_dots(e->cFileName))
			break;
	}

	n = WideCharToMultiByte(CP_UTF8, 0, e->cFileName, -1, name,
	    name_cap > 0x7FFFFFFFu ? 0x7FFFFFFF : (int)name_cap, NULL, NULL);
	if (n <= 0) {
		/* Too long for name: reported truncated and OTHER, as on Unix. */
		size_t cut = name_cap - 1u;

		name[cut] = '\0';
		*name_len = strlen(name);
		return 1;
	}
	*name_len = (size_t)n - 1u;
	if (e->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
		return 1;
	if (e->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
		*kind = NORMFS_DISK_SYS_DIR;
	} else {
		*kind = NORMFS_DISK_SYS_FILE;
		*size = ((uint64_t)e->nFileSizeHigh << 32) | e->nFileSizeLow;
	}
	return 1;
}

void
normfs_disk_sys_dir_close(void *handle)
{
	struct normfs_disk_sys_dir *d = handle;

	(void)FindClose(d->find);
	free(d);
}

int
normfs_disk_sys_file_size(const char *path, size_t path_len, uint64_t *size,
    int *os_error)
{
	wchar_t wide[NORMFS_DISK_PATH_MAX + 1];
	WIN32_FILE_ATTRIBUTE_DATA data;

	*os_error = 0;
	*size = 0u;
	if (normfs_disk_sys_widen(path, path_len, wide) < 0)
		return 0;
	if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &data)) {
		if (normfs_disk_sys_absent(GetLastError()))
			return 0;
		return normfs_disk_sys_fail(os_error);
	}
	if (data.dwFileAttributes &
	    (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
		return 0;
	*size = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
	return 1;
}

int
normfs_disk_sys_unlink(const char *path, size_t path_len, int *os_error)
{
	wchar_t wide[NORMFS_DISK_PATH_MAX + 1];

	*os_error = 0;
	if (normfs_disk_sys_widen(path, path_len, wide) < 0) {
		*os_error = ERROR_INVALID_NAME;
		return -1;
	}
	if (!DeleteFileW(wide))
		return normfs_disk_sys_fail(os_error);
	return 0;
}
