/*
 * fiwix/kernel/syscalls/chmod.c
 *
 * Copyright 2018, Jordi Sanfeliu. All rights reserved.
 * Distributed under the terms of the Fiwix License.
 */

#include <fiwix/types.h>
#include <fiwix/kernel.h>
#include <fiwix/fs.h>
#include <fiwix/stat.h>
#include <fiwix/errno.h>
#include <fiwix/string.h>

#ifdef __DEBUG__
#include <fiwix/stdio.h>
#include <fiwix/process.h>
#endif /*__DEBUG__ */

int sys_chmod(const char *filename, __mode_t mode)
{
	struct inode *i;
	char *tmp_name;
	int errno;

#ifdef __DEBUG__
	printk("(pid %d) sys_chmod('%s', %d)\n", current->pid, filename, mode);
#endif /*__DEBUG__ */

	if((errno = malloc_name(filename, &tmp_name)) < 0) {
		return errno;
	}
	if((errno = namei(tmp_name, &i, NULL, FOLLOW_LINKS))) {
		free_name(tmp_name);
		return errno;
	}

	if(IS_RDONLY_FS(i)) {
		iput(i);
		free_name(tmp_name);
		return -EROFS;
	}
	if(check_user_permission(i)) {
		iput(i);
		free_name(tmp_name);
		return -EPERM;
	}

	i->i_mode &= S_IFMT;
	i->i_mode |= mode & ~S_IFMT;
	i->i_ctime = CURRENT_TIME;
	i->state |= INODE_DIRTY;
	iput(i);
	free_name(tmp_name);
	return 0;
}
