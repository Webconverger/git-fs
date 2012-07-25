/*
 * fuse + libgit2 = read-only mounting of bare repos
 */

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <git2.h>

/* http://pubs.opengroup.org/onlinepubs/009695399/basedefs/limits.h.html
 */
#ifndef PATH_MAX
	#include <linux/limits.h>
#endif

git_repository *g_repo = NULL;
git_tree *g_tree = NULL;
git_index *g_index = NULL;
git_odb *g_odb;

char *gitfs_repo_path = NULL;
int enable_debug = 0;


#define error(...) fprintf(stderr, __VA_ARGS__)

void debug(const char* format, ...) {
	va_list args;
	if (!enable_debug)
		return;
	va_start(args,format);
	vfprintf(stderr, format, args);
	va_end(args);
}

git_index_entry *git_index_entry_by_file(const char *path) {
	unsigned int i, ecount;
	debug( "git_index_entry_by_file for %s\n", path);
	ecount = git_index_entrycount(g_index);
	for (i = 0; i < ecount; ++i) {
		git_index_entry *e = git_index_get(g_index, i);
		//debug( "  git_index_entry_by_file iterating over %s\n", e->path);

		if (! strcmp(e->path, path) == 0)
			continue;
		return e;
	}
	return NULL;
}

int is_dir(const char *path) {
	int ecount, i;

	debug("is_dir looking for '%s'\n", path);

	if (strcmp(path, "") == 0) {
		debug("  empty path is a dir, '%s'\n", path);
		return 1;
	}

	ecount = git_index_entrycount(g_index);
	for (i = 0; i < ecount; ++i) {
		git_index_entry *e = git_index_get(g_index, i);
		if (strncmp(path, e->path, strlen(path)) != 0) {
			//debug("  path '%s' not a substring of '%s'\n", path, e->path);
			continue;
		}
		if (e->path[ strlen(path) ] == '/') {
			debug("  prefix same, next char is slash for '%s' in '%s'\n", path, e->path);
			return 1;
		}
		if (strcmp(path, e->path) == 0) {
			debug("is_dir found file '%s'\n", e->path);
			return 0;
		}
	}
	debug("is_dir could not find path '%s'\n", path);
	return 0;
}

int git_getattr(const char *path, struct stat *stbuf)
{
	path++;
	memset(stbuf, 0, sizeof(struct stat));
	debug("getattr stripped to '%s'\n", path);
	if ( is_dir(path) ) {
		debug( "git_getattr for dir -> %s\n", path);
		stbuf->st_mode = 0040755;
		stbuf->st_nlink = 2;
		stbuf->st_gid = 0;
		stbuf->st_uid = 0;
		stbuf->st_size = 4096;
		return 0;
	}
	debug( "  get_attr not a dir %s\n", path);
	git_index_entry *e;
	if ( (e = git_index_entry_by_file(path)) == NULL)
		return debug("path no exist? %s\n", path),-ENOENT;

	stbuf->st_mode = e->mode;
	stbuf->st_gid = e->gid;
	stbuf->st_uid = e->uid;
	stbuf->st_nlink = 1;
	stbuf->st_size = e->file_size;
	return 0;
}

int git_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi)
{
	(void) offset;
	(void) fi;
	int i, ecount = 0;
	char dir[PATH_MAX], file[PATH_MAX], *p, *basename;

	path++; // drop initial slash
	debug( "git_readdir for %s\n", path);
	memset(dir, 0, sizeof(dir));
	ecount = git_index_entrycount(g_index);
	for (i = 0; i < ecount; ++i) {
		git_index_entry *e = git_index_get(g_index, i);
		memset(file, 0, sizeof(file));
		strncpy(file, e->path, strlen(e->path)+1);
		//debug( "  readdir for '%s' got '%s' from epath '%s'\n",path, file, e->path);
		if (strncmp(path, file, strlen(path)) != 0 && strlen(path) != 0) {
			//debug( "  readdir path '%s' not init substring of '%s'\n",path, file);
			continue;
		}
		basename = (char *)file + strlen(path);

		if ( strlen(path) > 0 &&  *basename != '/')
			continue;
		if ( strlen(path) > 0 &&  *basename == '/')
			basename++;

		debug( "  readdir using basename '%s'\n", basename);
		if ( (p = strchr(basename, '/')) == NULL) {
			debug( "  readdir path '%s' and obj '%s' is basename, adding\n", path, basename);
			filler(buf, basename, NULL, 0);
			continue;
		}
		if (strncmp(dir, basename, p - basename) != 0) {
			memset(dir, 0, sizeof(dir));
			strncpy(dir, basename, p - basename );
			debug( "  readdir path '%s' with basename '%s' and obj '%s' is new dir, adding\n", path, basename, dir);
			filler(buf, dir, NULL, 0);
		}
	}

	return 0;
}

int git_open(const char *path, struct fuse_file_info *fi)
{
	path++;
	return 0;
}

int git_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	(void) fi;
	git_index_entry *e;
	git_odb_object *obj;
	git_oid oid;

	path++;
	debug("git_read for -> %s with offset %d size %d\n", path, (int)offset, (int)size );

	if ( (e = git_index_entry_by_file(path)) == NULL)
		return debug("path no exist? %s\n", path),-ENOENT;

	oid = e->oid;
	git_odb_read(&obj, g_odb, &oid);
	debug( "git_read got %ld bytes\n", (long)(git_odb_object_size(obj)));

	memset(buf, 0, sizeof(buf));
	if (offset >= git_odb_object_size(obj)) {
		size = 0; goto ending;
	}
	if (offset + size > git_odb_object_size(obj))
		size = git_odb_object_size(obj) - offset;
	memcpy(buf, git_odb_object_data(obj) + offset, size);

	debug( "git_read copied %d bytes\n", (int)size);
ending:
	git_odb_object_free(obj);
	return size;
}

int git_readlink(const char *path, char *buf, size_t size) {
	struct fuse_file_info fi;
	int len = 0;

	memset(buf, 0, sizeof(buf));
	if ( (len = git_read(path, buf, size, 0, &fi)) > 0 ) {
		buf[len] = '\0';
		return 0;
	}
	return -ENOENT;
}

struct fuse_operations gitfs_oper = {
	.getattr= git_getattr,
	.readdir= git_readdir,
	.open= git_open,
	.read= git_read,
	.readlink= git_readlink
};

enum {
	KEY_DEBUG,
	KEY_RW,
};

static struct fuse_opt gitfs_opts[] = {
	FUSE_OPT_KEY("-d",             KEY_DEBUG),
	FUSE_OPT_KEY("debug",          KEY_DEBUG),
	FUSE_OPT_KEY("rw",             KEY_RW),
	FUSE_OPT_END
};

static int gitfs_opt_proc(void *data, const char *arg, int key, struct fuse_args *outargs)
{
	/* The first non-option argument is the repo path */
	if (key == FUSE_OPT_KEY_NONOPT && gitfs_repo_path == NULL) {
		gitfs_repo_path = strdup(arg);
		/* Don't pass this option onto fuse_main */
		return 0;
	} else if (key == KEY_DEBUG) {
		enable_debug = 1;
		/* Pass this option onto fuse_main */
		return 1;
	} else if (key == KEY_RW) {
		error("Mounting read-write not supported, ignoring rw option\n");
		/* Don't pass this option onto fuse_main */
		return 0;
	}

	/* Pass all other options to fuse_main */
	return 1;
}

int main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct stat st;

	if (fuse_opt_parse(&args, NULL, gitfs_opts, gitfs_opt_proc))
		return error("Invalid arguments\n"), 1;

	if (gitfs_repo_path == NULL)
		return error("No repository path given\n"), 1;

	if (stat(gitfs_repo_path, &st) < 0 || !S_ISDIR(st.st_mode))
		return error("%s: path does not exist?", gitfs_repo_path), 1;

	if (git_repository_open(&g_repo, gitfs_repo_path) < 0)
		return perror("repo"), 1;

	git_repository_index(&g_index, g_repo);
	git_index_read(g_index);
	git_repository_odb(&g_odb, g_repo);


	return fuse_main(args.argc, args.argv, &gitfs_oper);
}

