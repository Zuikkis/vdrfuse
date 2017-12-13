/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
  Copyright (C) 2011       Sebastian Pipping <sebastian@pipping.org>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

/* VDR fuse filesystem for Plex by Teemu Suikki

   You probably want to start with the subdir module like this:
   vdrfuse -o modules=subdir,subdir=/srv/vdr/video  mntpoint

*/

#define FUSE_USE_VERSION 31

#define _GNU_SOURCE

#include <fuse.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/file.h> /* flock(2) */


struct myfileinfo {
    int fh;             // actual file
    int numcuts;        // max index of cutbuf. Zero for regular files
    uint64_t cutbuf[];
};

struct tIndexTs {
  uint64_t offset:40; // up to 1TB per file (not using off_t here - must definitely be exactly 64 bit!)
  int reserved:7;     // reserved for future use
  int independent:1;  // marks frames that can be displayed by themselves (for trick modes)
  uint16_t number:16; // up to 64K files per recording
};

// read maximum of 11 characters to buf, and skip to next newline
static int myfgets(FILE *fh, char *buf)
{
    int i,x=0;

    do {
        i=fgetc(fh);
        if (x<11) *buf++=i;
        x++;
    } while ((i!=EOF) && (i!=10));

    if ((buf[-1]==EOF) || (buf[-1]==10)) buf[-1]=0; else buf[0]=0;

    if (i==EOF) return 0; else return 1;
}

static int getframenum(char *buf, int fps) {
// accepted format:  0:07:01.20 comment
    int h, m, s, f;

    if (sscanf(buf,"%d:%d:%d.%d",&h,&m,&s,&f)!=4)
    {
        f=0;
        if (sscanf(buf,"%d:%d:%d",&h,&m,&s)!=3) return -1;
    }

    m+=h*60;
    s+=m*60;
    f+=s*fps;

    return f;
}

static struct myfileinfo *mfi_create(const char *path) {
    struct myfileinfo *mfi=NULL;
    const char *filename;

    printf("mfi_create: %s\n", path);

    filename=strrchr(path,'/');
    if (filename)
        filename++;
    else
        filename=path;

    // See if this file needs special attention
    if (!strcmp(filename,"00001.ts")) {
        FILE *marksfh, *indexfh, *infofh;
        int i, lines=0, fps=0, frame;
        char *s,*p, buf[12];
        struct tIndexTs tits;

        i=filename-path;
        s=malloc(i+10);
        if (!s) return NULL;
        strcpy(s,path);
        p=s+i;

        // 00001.ts needs reading of marks, index and info. this is huge
        // if either of those is missing, it's not an error. Instead just open as regular file
        strcpy(p,"marks");
        marksfh=fopen(s,"rb");
        strcpy(p,"index");
        indexfh=fopen(s,"rb");
        strcpy(p,"info");
        infofh=fopen(s,"rb");
        free(s);

        if (marksfh && indexfh && infofh) {
            // first read fps value from info
            while (myfgets(infofh, buf)) {
                if ((buf[0]=='F') && (buf[1]==' ')) fps=atoi(buf+2);
            }

            // calculate number of lines in marksfh. Also check that each value is valid!
            i=1;
            while (myfgets(marksfh, buf)) {
                lines++;
                if (getframenum(buf, fps)==-1) i=0;
            }

            if (i && fps) {
                rewind(marksfh);
                // must be even, so round up
                lines = (lines+1)&0xfffffffe;

                mfi=malloc(sizeof(struct myfileinfo) + lines*(sizeof(int64_t)));
                if (mfi) {
                    int diff, bestdiff, fail=0, prev=-1;

                    memset(mfi, 0, sizeof(struct myfileinfo) + lines*(sizeof(int64_t)));
                    mfi->numcuts=lines;

                    lines=0;
                    // read marks and convert to frame number
                    while (myfgets(marksfh, buf) && (lines<mfi->numcuts)) {
                        frame=getframenum(buf, fps);
                        printf("offset: %d\n",frame);

                        // only accept marks in ascending order
                        if (frame<prev) fail=1;
                        prev=frame;

                        i=frame-100;
                        if (i<0) i=0;
                        bestdiff=10000;
                        if (fseek(indexfh, i*8, SEEK_SET) != -1) {
                            do {
                                if (fread(&tits, 8, 1, indexfh) && tits.independent) {
                                    diff=abs(frame-i);
                                    if (diff<bestdiff) {
                                        bestdiff=diff;
                                        mfi->cutbuf[lines]=tits.offset;
                                        if (tits.number!=1) fail=10;
                                    }
                                }
                                i++;
                            } while (i-frame<100);
                            if (bestdiff==10000) fail=1;
                            printf("diff: %d, moviepos: %ld\n",bestdiff,mfi->cutbuf[lines]);
                            lines++;
                        } else fail=2;
                    }
                    // missing final closing mark?
                    if (lines<mfi->numcuts) {
                        if (fseek(indexfh, -8, SEEK_END) != -1) {
                            if (fread(&tits, 8, 1, indexfh)) {
                                mfi->cutbuf[lines]=tits.offset;
                            } else fail=3;
                            printf("final moviepos: %ld\n",mfi->cutbuf[lines]);
                        } else fail=4;
                    }

                    // if errors, change to normal file
                    if (fail) {
                        printf("fail: %d\n", fail);
                        mfi->numcuts=0;
                    }
                }
            } else {
                printf("invalid marks file, not used\n");
            }
        }

        if (marksfh) fclose(marksfh);
        if (indexfh) fclose(indexfh);
        if (infofh) fclose(infofh);
    }
    if (!mfi) {
        mfi=malloc(sizeof(struct myfileinfo));
        mfi->numcuts=0;
    }
    return mfi;
}

// get total file size
static uint64_t mfi_size(struct myfileinfo *mfi) {
    int i;
    uint64_t size=0;

    for (i=0;i<mfi->numcuts;i+=2) {
        size+=(mfi->cutbuf[i+1]-mfi->cutbuf[i]);
    }
    return size;
}

static void *xmp_init(struct fuse_conn_info *conn,
		      struct fuse_config *cfg)
{
	(void) conn;
	cfg->use_ino = 1;
	cfg->nullpath_ok = 1;

	/* Pick up changes from lower filesystem right away. This is
	   also necessary for better hardlink support. When the kernel
	   calls the unlink() handler, it does not know the inode of
	   the to-be-removed entry and can therefore not invalidate
	   the cache of the associated inode - resulting in an
	   incorrect st_nlink value being reported for any remaining
	   hardlinks to this inode. */
	cfg->entry_timeout = 0;
	cfg->attr_timeout = 0;
	cfg->negative_timeout = 0;

	return NULL;
}

static int xmp_getattr(const char *path, struct stat *stbuf,
			struct fuse_file_info *fi)
{
	int res=-1;
    struct myfileinfo *mfi;
    if (fi) mfi=(struct myfileinfo *)fi->fh;

	(void) path;

    printf("xmp_getattr: %s\n", path);

    if(fi) {
	    res = fstat(mfi->fh, stbuf);
        if (mfi->numcuts && !res) {
            printf("real size: %ld\n",stbuf->st_size);
            stbuf->st_size=mfi_size(mfi);               // fix size
            printf("new size: %ld\n",stbuf->st_size);
        }
	} else {
        if (path) {
            const char *filename;
            filename=strrchr(path,'/');
            if (filename)
                filename++;
            else
                filename=path;

            // See if this file needs special attention
            if (!strcmp(filename,"00001.ts")) {
                mfi=mfi_create(path);
                res = lstat(path, stbuf);
                if (mfi->numcuts && !res) {
                    printf("real size: %ld\n",stbuf->st_size);
                    stbuf->st_size=mfi_size(mfi);               // fix size
                    printf("new size: %ld\n",stbuf->st_size);
                }
                free(mfi);
            } else {
                res = lstat(path, stbuf);
            }
        }
    }
	if (res == -1)
	    return -errno;
   	return 0;
}

static int xmp_access(const char *path, int mask)
{
	int res;

    printf("xmp_access: %s\n", path);

	res = access(path, mask);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_readlink(const char *path, char *buf, size_t size)
{
	int res;

    printf("xmp_readlink: %s\n", path);

	res = readlink(path, buf, size - 1);
	if (res == -1)
		return -errno;

	buf[res] = '\0';
	return 0;
}

struct xmp_dirp {
	DIR *dp;
	struct dirent *entry;
	off_t offset;
};

static int xmp_opendir(const char *path, struct fuse_file_info *fi)
{
	int res;
	struct xmp_dirp *d = malloc(sizeof(struct xmp_dirp));

    printf("xmp_opendir: %s\n", path);

	if (d == NULL)
		return -ENOMEM;

	d->dp = opendir(path);
	if (d->dp == NULL) {
		res = -errno;
		free(d);
		return res;
	}
	d->offset = 0;
	d->entry = NULL;

	fi->fh = (unsigned long) d;
	return 0;
}

static inline struct xmp_dirp *get_dirp(struct fuse_file_info *fi)
{
	return (struct xmp_dirp *) (uintptr_t) fi->fh;
}

static int xmp_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		       off_t offset, struct fuse_file_info *fi,
		       enum fuse_readdir_flags flags)
{
	struct xmp_dirp *d = get_dirp(fi);

	(void) path;

    printf("xmp_readdir: %s\n", path);

	if (offset != d->offset) {
#ifndef __FreeBSD__
		seekdir(d->dp, offset);
#else
		/* Subtract the one that we add when calling
		   telldir() below */
		seekdir(d->dp, offset-1);
#endif
		d->entry = NULL;
		d->offset = offset;
	}
	while (1) {
		struct stat st;
		off_t nextoff;
		enum fuse_fill_dir_flags fill_flags = 0;

		if (!d->entry) {
			d->entry = readdir(d->dp);
			if (!d->entry)
				break;
		}
#ifdef HAVE_FSTATAT
		if (flags & FUSE_READDIR_PLUS) {
			int res;

			res = fstatat(dirfd(d->dp), d->entry->d_name, &st,
				      AT_SYMLINK_NOFOLLOW);
			if (res != -1)
				fill_flags |= FUSE_FILL_DIR_PLUS;
		}
#endif
		if (!(fill_flags & FUSE_FILL_DIR_PLUS)) {
			memset(&st, 0, sizeof(st));
			st.st_ino = d->entry->d_ino;
			st.st_mode = d->entry->d_type << 12;
		}
		nextoff = telldir(d->dp);
#ifdef __FreeBSD__		
		/* Under FreeBSD, telldir() may return 0 the first time
		   it is called. But for libfuse, an offset of zero
		   means that offsets are not supported, so we shift
		   everything by one. */
		nextoff++;
#endif

		if (filler(buf, d->entry->d_name, &st, nextoff, fill_flags))
			break;

		d->entry = NULL;
		d->offset = nextoff;
	}

	return 0;
}

static int xmp_releasedir(const char *path, struct fuse_file_info *fi)
{
	struct xmp_dirp *d = get_dirp(fi);
	(void) path;

    printf("xmp_releasedir: %s\n", path);

	closedir(d->dp);
	free(d);
	return 0;
}

static int xmp_mknod(const char *path, mode_t mode, dev_t rdev)
{
	int res;

    printf("xmp_mknod: %s\n", path);

	if (S_ISFIFO(mode))
		res = mkfifo(path, mode);
	else
		res = mknod(path, mode, rdev);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_mkdir(const char *path, mode_t mode)
{
	int res;

    printf("xmp_mkdir: %s\n", path);

	res = mkdir(path, mode);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_unlink(const char *path)
{
	int res;

    printf("xmp_unlink: %s\n", path);

	res = unlink(path);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_rmdir(const char *path)
{
	int res;

    printf("xmp_rmdir: %s\n", path);

	res = rmdir(path);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_symlink(const char *from, const char *to)
{
	int res;

	res = symlink(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_rename(const char *from, const char *to, unsigned int flags)
{
	int res;

	/* When we have renameat2() in libc, then we can implement flags */
	if (flags)
		return -EINVAL;

	res = rename(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_link(const char *from, const char *to)
{
	int res;

	res = link(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_chmod(const char *path, mode_t mode,
		     struct fuse_file_info *fi)
{
	int res;
    struct myfileinfo *mfi;
    if (fi) mfi=(struct myfileinfo *)fi->fh;

    printf("xmp_chmod: %s\n", path);

	if(fi)
		res = fchmod(mfi->fh, mode);
	else
		res = chmod(path, mode);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_chown(const char *path, uid_t uid, gid_t gid,
		     struct fuse_file_info *fi)
{
	int res;
    struct myfileinfo *mfi;
    if (fi) mfi=(struct myfileinfo *)fi->fh;

    printf("xmp_chown: %s\n", path);

	if (fi)
		res = fchown(mfi->fh, uid, gid);
	else
		res = lchown(path, uid, gid);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_truncate(const char *path, off_t size,
             struct fuse_file_info *fi)
{
    int res;
    struct myfileinfo *mfi;

    if(fi) {
        mfi=(struct myfileinfo *)fi->fh;
        if (mfi->numcuts) return -ENOSYS;
        res = ftruncate(mfi->fh, size);
    } else {
        res = truncate(path, size);
    }

    if (res == -1)
        return -errno;

    return 0;
}

static int xmp_utimens(const char *path, const struct timespec ts[2],
		       struct fuse_file_info *fi)
{
	int res;
    struct myfileinfo *mfi;
    if (fi) mfi=(struct myfileinfo *)fi->fh;

	/* don't use utime/utimes since they follow symlinks */
	if (fi)
		res = futimens(mfi->fh, ts);
	else
		res = utimensat(0, path, ts, AT_SYMLINK_NOFOLLOW);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	int fd;
    struct myfileinfo *mfi;

    printf("xmp_create: %s\n", path);

	fd = open(path, fi->flags, mode);
	if (fd == -1)
		return -errno;

    mfi=mfi_create(path);
    if (!mfi) {
        close(fd);
        return -ENOMEM;
    }

	mfi->fh = fd;
	fi->fh = (unsigned long) mfi;
	return 0;
}

static int xmp_open(const char *path, struct fuse_file_info *fi)
{
	int fd;
    struct myfileinfo *mfi;

    printf("xmp_open: %s\n", path);

	fd = open(path, fi->flags);
	if (fd == -1)
		return -errno;

    mfi=mfi_create(path);
    if (!mfi) {
        close(fd);
        return -ENOMEM;
    }

	mfi->fh = fd;
	fi->fh = (unsigned long) mfi;
	return 0;
}

static int xmp_read(const char *path, char *buf, size_t size, off_t offset,
		    struct fuse_file_info *fi)
{
	int res, bytes=0;
    struct myfileinfo *mfi=(struct myfileinfo *)fi->fh;

	(void) path;

    printf("xmp_read: %s, size %ld, offset %ld\n", path, size, offset);

    if (!mfi->numcuts) {
	    bytes = res = pread(mfi->fh, buf, size, offset);
    } else {
        uint64_t cutsize, len;
        int cnt=0;

        do {
            cutsize=mfi->cutbuf[cnt+1]-mfi->cutbuf[cnt];
            printf("cutsize: %ld, offset: %ld\n",cutsize,offset);
            if (cutsize>offset) {
                len=cutsize-offset;
                printf("len: %ld, size: %ld\n",len, size);
                if (len>=size) {
        	        res = pread(mfi->fh, buf, size, mfi->cutbuf[cnt]+offset);
                    printf("read %d bytes\n", res);
                    if (res>0) bytes+=res;
                    goto done;
                }
        	    res = pread(mfi->fh, buf, len, mfi->cutbuf[cnt]+offset);
                printf("read %d bytes\n", res);
                if (res>0) bytes+=res;
                if (res<len) goto done;
                buf+=len;
                size-=len;
                offset+=len;
            }
            cnt+=2;
            offset-=cutsize;
        }
        while ((cnt<mfi->numcuts) && (offset>=0));
    }

done:
    if (res == -1)
	    bytes = -errno;

   printf("Total %d bytes\n", bytes);

	return bytes;
}

static int xmp_write(const char *path, const char *buf, size_t size,
		     off_t offset, struct fuse_file_info *fi)
{
	int res;
    struct myfileinfo *mfi=(struct myfileinfo *)fi->fh;

	(void) path;
	res = pwrite(mfi->fh, buf, size, offset);
	if (res == -1)
		res = -errno;

	return res;
}

static int xmp_statfs(const char *path, struct statvfs *stbuf)
{
	int res;

    printf("xmp_statfs: %s\n", path);

	res = statvfs(path, stbuf);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_flush(const char *path, struct fuse_file_info *fi)
{
	int res;
    struct myfileinfo *mfi;
    if (fi) mfi=(struct myfileinfo *)fi->fh;


    printf("xmp_flush: %s\n", path);

	(void) path;
	/* This is called from every close on an open file, so call the
	   close on the underlying filesystem.	But since flush may be
	   called multiple times for an open file, this must not really
	   close the file.  This is important if used on a network
	   filesystem like NFS which flush the data/metadata on close() */
	res = close(dup(mfi->fh));
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_release(const char *path, struct fuse_file_info *fi)
{
	(void) path;
    struct myfileinfo *mfi=(struct myfileinfo *)fi->fh;

    printf("xmp_release: %s\n", path);

	close(mfi->fh);
    free(mfi);
	fi->fh = (unsigned long) NULL;
	return 0;
}

static int xmp_fsync(const char *path, int isdatasync,
		     struct fuse_file_info *fi)
{
	int res;
	(void) path;
    struct myfileinfo *mfi=(struct myfileinfo *)fi->fh;

    printf("xmp_fsync: %s\n", path);

#ifndef HAVE_FDATASYNC
	(void) isdatasync;
#else
	if (isdatasync)
		res = fdatasync(mfi->fh);
	else
#endif
		res = fsync(mfi->fh);
	if (res == -1)
		return -errno;

	return 0;
}

#ifdef HAVE_POSIX_FALLOCATE
static int xmp_fallocate(const char *path, int mode,
			off_t offset, off_t length, struct fuse_file_info *fi)
{
	(void) path;
    struct myfileinfo *mfi=(struct myfileinfo *)fi->fh;

    printf("xmp_fallocate: %s\n", path);

	if (mode)
		return -EOPNOTSUPP;

	return -posix_fallocate(mfi->fh, offset, length);
}
#endif

static int xmp_flock(const char *path, struct fuse_file_info *fi, int op)
{
	int res;
	(void) path;
    struct myfileinfo *mfi=(struct myfileinfo *)fi->fh;

    printf("xmp_flock: %s\n", path);

	res = flock(mfi->fh, op);
	if (res == -1)
		return -errno;

	return 0;
}

static struct fuse_operations xmp_oper = {
	.init           = xmp_init,
	.getattr	= xmp_getattr,
	.access		= xmp_access,
	.readlink	= xmp_readlink,
	.opendir	= xmp_opendir,
	.readdir	= xmp_readdir,
	.releasedir	= xmp_releasedir,
	.mknod		= xmp_mknod,
	.mkdir		= xmp_mkdir,
	.symlink	= xmp_symlink,
	.unlink		= xmp_unlink,
	.rmdir		= xmp_rmdir,
	.rename		= xmp_rename,
	.link		= xmp_link,
	.chmod		= xmp_chmod,
	.chown		= xmp_chown,
    .truncate   = xmp_truncate,
	.utimens	= xmp_utimens,
	.create		= xmp_create,
	.open		= xmp_open,
	.read		= xmp_read,
	.write		= xmp_write,
	.statfs		= xmp_statfs,
	.flush		= xmp_flush,
	.release	= xmp_release,
	.fsync		= xmp_fsync,
#ifdef HAVE_POSIX_FALLOCATE
	.fallocate	= xmp_fallocate,
#endif
	.flock		= xmp_flock,
};

int main(int argc, char *argv[])
{
	umask(0);
	return fuse_main(argc, argv, &xmp_oper, NULL);
}
