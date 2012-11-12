/*
 * fuse + libgit2 = read-only mounting of bare repos
 */

#define FUSE_USE_VERSION 25
#define _FILE_OFFSET_BITS 64
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <git2.h>
#include <unistd.h>

/* http://pubs.opengroup.org/onlinepubs/009695399/basedefs/limits.h.html
 */
#ifndef PATH_MAX
	#include <linux/limits.h>
#endif

/* Macro to get the length of a static array */
#define lengthof(arr) (sizeof(arr) / sizeof(*arr))

char *gitfs_repo_path = NULL;
char *gitfs_rev = NULL;
git_oid gitfs_tree_oid;
time_t gitfs_commit_time;
int enable_debug = 0;
int retval;

#define error(...) fprintf(stderr, __VA_ARGS__)

/* Macro to hide the ugly casts needed to access fi->fh (which is a
 * uint64_t, which can store pointers, but is too big on 32-bit systems,
 * requiring multiple casts to silence the compiler warnings). */
#define GITFS_FH(fi) ((gitfs_entry *)(intptr_t)fi->fh)

typedef enum {
	GITFS_FILE,
	GITFS_DIR,
} gitfs_entry_type;

void debug(const char* format, ...) {
	va_list args;
	if (!enable_debug)
		return;
	va_start(args,format);
	vfprintf(stderr, format, args);
	va_end(args);
}

typedef struct gitfs_entry {
	/* The tree_entry for this entry, or NULL for the root directory */
	git_tree_entry *tree_entry;
	/** The type */
	gitfs_entry_type type;
	/* The tree or blob corresponding to this
	 * entry */
	union {
		git_tree *tree;
		git_blob *blob;
	} object;
} gitfs_entry;

struct gitfs_data {
	git_repository *repo;
	git_tree *tree;
};

void gitfs_entry_free(gitfs_entry *e) {
	if (e->type == GITFS_DIR && e->tree_entry != NULL)
		/* Don't free the tree when tree_entry is NULL, since
		 * that's the root tree */
		git_tree_free(e->object.tree);
	else if (e->type == GITFS_FILE)
		git_blob_free(e->object.blob);

	if (e->tree_entry)
		git_tree_entry_free(e->tree_entry);

	free(e);
}

int gitfs_lookup_entry(gitfs_entry **out, const char *path) {
	struct gitfs_data *d = (struct gitfs_data *)(fuse_get_context()->private_data);
	int retval = 0;

	gitfs_entry *e = *out = calloc(1, sizeof(gitfs_entry));
	if (!e) {
		error("Failed to allocate memory for entry: '%s'\n", path);
		retval = -ENOMEM;
		goto out;
	}

	if (path[0] == '/' && path[1] == '\0') {
		/* We can't use git_tree_entry_bypath for the root path,
		 * so short circuit here. Also set out to 0 to signal
		 * this special case (there exists no git_tree_entry for
		 * the root path since the root path is not an entry in
		 * any other tree). */
		e->tree_entry = NULL;
		e->type = GITFS_DIR;
		e->object.tree = d->tree;
		return 0;
	}
	/* Fill e->tree_entry */
	if (git_tree_entry_bypath(&e->tree_entry, d->tree, path + 1) < 0) {
		debug("File not found: '%s'\n", path);
		retval = -ENOENT;
		goto out;
	}

	/* Fill e->type */
	switch(git_tree_entry_type(e->tree_entry)) {
		case GIT_OBJ_TREE:
			/* Lookup the corresponding git_tree object and
			 * store it into e->object */
			if (git_tree_entry_to_object((git_object**)&e->object.tree, d->repo, e->tree_entry) < 0) {
				error("Tree not found?!: '%s'\n", path);
				retval = -EIO;
				goto out;
			}
			e->type = GITFS_DIR;
			break;

		case GIT_OBJ_BLOB:
			/* Lookup the corresponding git_blob object and
			 * store it into e->object */
			if (git_tree_entry_to_object((git_object**)&e->object.blob, d->repo, e->tree_entry) < 0) {
				error("Blob not found?!: '%s'\n", path);
				retval = -EIO;
				goto out;
			}
			e->type = GITFS_FILE;
			break;

		case GIT_OBJ_COMMIT:
			debug("Ignoring submodule entry: '%s'\n", path);
			retval = -ENOENT;
			goto out;

		default:
			debug("Ignoring unknown entry: '%s'\n", path);
			retval = -ENOENT;
			goto out;
	}
out:
	if (retval < 0 && e) {
		gitfs_entry_free(e);
		*out = 0;
	}
	return retval;
}

int gitfs_open(const char *path, struct fuse_file_info *fi)
{
	/* Find the corresponding entry and store it inside the fh
	 * member, for use in other operations. */
	return gitfs_lookup_entry((gitfs_entry**)&fi->fh, path);
}

int gitfs_release(const char *path, struct fuse_file_info *fi)
{
	/* Free the tree_entry pointer in fh */
	if(fi->fh)
		gitfs_entry_free(GITFS_FH(fi));
	return 0;
}

int gitfs_getattr(const char *path, struct stat *stbuf)
{
	int retval = 0;
	debug("Getattr called for '%s'\n", path);
	gitfs_entry *e = NULL;
	if ((retval = gitfs_lookup_entry(&e, path)) < 0)
		goto out;

	memset(stbuf, 0, sizeof(struct stat));

	/* Set all times to the only time we (might) have available: The
	 * time the commit we're working with was made. Note that we
	 * _could_ search back through history to find the real times,
	 * of files, but this is time-consuming and probably not worth
	 * the trouble (right now). */
	stbuf->st_atime = gitfs_commit_time;
	stbuf->st_ctime = gitfs_commit_time;
	stbuf->st_mtime = gitfs_commit_time;

	if (e->type == GITFS_DIR) {
		debug( "Path is a directory: '%s'\n", path);
		stbuf->st_nlink = 2;
		stbuf->st_mode = 040755;
		stbuf->st_size = 4096;
	} else if (e->type == GITFS_FILE) {
		debug( "Path is a file: '%s'\n", path);
		stbuf->st_nlink = 1;
		stbuf->st_mode = git_tree_entry_attributes(e->tree_entry);
		/* Override the permissions for links, since git just
		 * stores the link type bit. */
		if (S_ISLNK(stbuf->st_mode))
			stbuf->st_mode = S_IFLNK | S_IRWXU | S_IRWXG | S_IRWXO;
		/* Note that this gives the length of the filename for
		 * symlinks, but that's what native filesystems do as
		 * well. */
		stbuf->st_size = git_blob_rawsize(e->object.blob);
	} else {
		error("Unsupported type?!\n");
		retval = -EIO;
		goto out;
	}

	stbuf->st_gid = 0;
	stbuf->st_uid = 0;

out:
	if (e)
		gitfs_entry_free(e);

	return retval;
}

int gitfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi)
{
	debug("readdir called for '%s'\n", path);
	gitfs_entry *e = GITFS_FH(fi);
	if (e->type != GITFS_DIR)
		return debug("Path is not a directory?!: '%s'\n", path), -EIO;

	int entry_count = git_tree_entrycount(e->object.tree);
	while (offset < entry_count) {
		const git_tree_entry *entry = git_tree_entry_byindex(e->object.tree, offset);
		/* Add the entry to the list. The offset passed is the
		 * offset to the _next_ entry. If filler returns 1, buf
		 * is full, and we should stop trying to add entries.
		 * Note that the the last entry is _not_ added in this
		 * case. Future calls readdir will have offset
		 * appropriately set to the value passed to filler with
		 * the last successful addition. */
		if (filler(buf, git_tree_entry_name(entry), NULL, offset + 1) == 1)
			return 0;
		offset++;
	}

	return 0;
}

int gitfs_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	debug("read called for '%s' (offset %d, size %d)\n", path, offset, size);
	gitfs_entry *e = GITFS_FH(fi);
	if (e->type != GITFS_FILE || !S_ISREG(git_tree_entry_attributes(e->tree_entry)))
		return error("Path is not a file?!: '%s'\n", path), -EIO;

	int blob_size = git_blob_rawsize(e->object.blob);

	if (offset >= blob_size)
		size = 0;
	else if (offset + size > blob_size)
		size = blob_size - offset;

	if (size)
		memcpy(buf, git_blob_rawcontent(e->object.blob) + offset, size);

	debug( "read copied %d bytes\n", (int)size);
	return size;
}

int gitfs_readlink(const char *path, char *buf, size_t size) {
	int retval = 0;
	debug("read called for '%s'\n", path);
	gitfs_entry *e = NULL;

	/* Sanity checks */
	if ((retval = gitfs_lookup_entry(&e, path)) < 0)
		goto out;

	if (e->type != GITFS_FILE || !S_ISLNK(git_tree_entry_attributes(e->tree_entry))) {
		debug("Path is not a link?!: '%s'\n", path);
		retval = -EIO;
		goto out;
	}

	int blob_size = git_blob_rawsize(e->object.blob);

	/* If the blob is too big for buf (keeping room for the trailing
	 * NUL), truncate (as per fuse docs) */
	if (blob_size  > size - 1)
		blob_size = size - 1;

	memcpy(buf, git_blob_rawcontent(e->object.blob), blob_size);
	buf[blob_size] = '\0';

out:
	if (e)
		gitfs_entry_free(e);
	return retval;
}

void gitfs_destroy(void *private_data) {
	struct gitfs_data *d = (struct gitfs_data *)private_data;

	if (d) {
		if (d->tree) git_tree_free(d->tree);
		if (d->repo) git_repository_free(d->repo);
		free(d);
	}
}

void* gitfs_init(void) {
	char sha[GIT_OID_HEXSZ + 1];
	/* Start by chrooting into the git repository. Doing this allows
	 * git-fs to be started from within initrd and not break if
	 * mount points are shuffled around, causing the location of the
	 * git repository to change. By chrooting into the .git dir,
	 * anything can happen, except for unmounting it completely.
	 * Note that we can't do this chroot in main(), since fuse_main
	 * needs /dev/fuse and possibly /dev/null and others too... */
	debug("chrooting to %s\n", gitfs_repo_path);

	if (chroot(gitfs_repo_path) < 0) {
		error("Failed to chroot to %s: %s\n", gitfs_repo_path, strerror(errno));
		goto err;
	}
	if (chdir("/") < 0) {
		error("Failed to chdir to /: %s\n", strerror(errno));
		goto err;
	}

	struct gitfs_data *d = calloc(1, sizeof(struct gitfs_data));
	if (!d) {
		error("Failed to allocate memory for userdata\n");
		goto err;
	}

	debug("opening repo after fuse_main\n");
	if (git_repository_open(&d->repo, "/") < 0) {
		error("Cannot open git repository: %s\n", giterr_last()->message);
		goto err;
	}

	if (git_tree_lookup(&d->tree, d->repo, &gitfs_tree_oid) < 0) {
		git_oid_fmt(sha, &gitfs_tree_oid);
		sha[GIT_OID_HEXSZ] = '\0';
		error("Failed to lookup tree: %s\n", sha);
		goto err;
	}

	/* This return value can be accessed through
	 * fuse_get_context()->private_data */
	return (void*)d;

err:
	gitfs_destroy((void*)d);

	/* Tell fuse to exit the mainloop (doesn't exit immediately) */
	fuse_exit(fuse_get_context()->fuse);

	/* Store a return value, so we don't return success when the
	 * mounting failed */
	retval = 1;
	return NULL;
}

struct fuse_operations gitfs_oper = {
	.init= gitfs_init,
	.destroy= gitfs_destroy,
	.open= gitfs_open,
	.release= gitfs_release,
	/* Reuse open/release for directories */
	.opendir= gitfs_open,
	.releasedir= gitfs_release,
	.getattr= gitfs_getattr,
	.readdir= gitfs_readdir,
	.read= gitfs_read,
	.readlink= gitfs_readlink
};

void usage(struct fuse_args *args, FILE *out) {
	fprintf(out,
	     "usage: %s [options] repo-path mountpoint\n"
	     "\n"
	     "Mount the repository in repo-path onto mountpoint.\n"
	     "repo-path should point to the .git directory, not the\n"
	     "checkout directory (can also point to a bare repository).\n"
	     "\n"
	     "general options:\n"
	     "    -o opt,[opt...]\n"
	     "        mount options (see below)\n"
	     "    -h\n"
	     "    --help\n"
	     "        print help\n"
	     "\n"
	     "git-fs options:\n"
	     "    -o rev=STRING\n"
	     "    --rev=STRING\n"
	     "        Revision to mount. Can be any name that points to\n"
	     "        a commit or tree object (e.g. a branch name, tag\n"
	     "        name, symbolic ref, sha). When not specified,\n"
	     "        HEAD is used.\n"
	     "\n"
	     , args->argv[0]);
             fuse_opt_add_arg(args, "-ho");
             fuse_main(args->argc, args->argv, &gitfs_oper);
}

enum {
	KEY_DEBUG,
	KEY_HELP,
	KEY_REV,
	KEY_RWRO,
};

static struct fuse_opt gitfs_opts[] = {
	FUSE_OPT_KEY("-d",             KEY_DEBUG),
	FUSE_OPT_KEY("debug",          KEY_DEBUG),
	FUSE_OPT_KEY("-h",             KEY_HELP),
	FUSE_OPT_KEY("--help",         KEY_HELP),
	FUSE_OPT_KEY("--rev=%s",       KEY_REV),
	FUSE_OPT_KEY("rev=%s",         KEY_REV),
	FUSE_OPT_KEY("rw",             KEY_RWRO),
	FUSE_OPT_KEY("ro",             KEY_RWRO),
	FUSE_OPT_END
};

static int gitfs_opt_proc(void *data, const char *arg, int key, struct fuse_args *outargs)
{
	/* The first non-option argument is the repo path */
	if (key == FUSE_OPT_KEY_NONOPT && gitfs_repo_path == NULL) {
		gitfs_repo_path = realpath(arg, NULL);
		if (gitfs_repo_path == NULL) {
			error("%s: Failed to resolve path: %s\n", arg, strerror(errno));
			return -1;
		}
		/* Don't pass this option onto fuse_main */
		return 0;
	} else if (key == KEY_DEBUG) {
		enable_debug = 1;
		/* Pass this option onto fuse_main */
		return 1;
	} else if (key == KEY_RWRO) {
		error("Mount is always read-only, ignoring %s option\n", arg);
		/* Don't pass this option onto fuse_main */
		return 0;
	} else if (key == KEY_REV) {
		if (gitfs_rev != NULL) {
			error("--rev / -o rev can be passed only once\n");
			return -1;
		}
		gitfs_rev = strdup(strchr(arg, '=') + 1);
		/* Don't pass this option onto fuse_main */
		return 0;
	} else if (key == KEY_HELP) {
		usage(outargs, stdout);
		exit(0);
	}

	/* Pass all other options to fuse_main */
	return 1;
}

int main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct stat st;
	char sha[41];

	if (fuse_opt_parse(&args, NULL, gitfs_opts, gitfs_opt_proc))
		return 1;

	if (gitfs_repo_path == NULL)
		return error("No repository path given\n\n"), usage(&args, stderr), 1;

	if (stat(gitfs_repo_path, &st) < 0 || !S_ISDIR(st.st_mode))
		return error("%s: path does not exist?\n", gitfs_repo_path), 1;

	/* We open the repo now and resolve the arguments given, so we
	 * can bail out and provide an error message when anything is
	 * wrong. We'll have to re-open the repository later in
	 * gitfs_init after the chroot, since the chroot will break the
	 * repository object (but once we are there, we might have
	 * already detached from the terminal, so it's too late to
	 * provide useful error messages). */
	debug("opening repo before fuse_main\n");
	git_repository *repo;
	if (git_repository_open(&repo, gitfs_repo_path) < 0)
		return error("Cannot open git repository: %s\n", giterr_last()->message), 1;

	/* Default to HEAD */
	char *rev = "HEAD";
	if (gitfs_rev)
		rev = gitfs_rev;
	debug("using rev %s\n", rev);

	git_object *obj;
	if (git_revparse_single(&obj, repo, rev) < 0)
		return error("Failed to resolve rev: %s\n", rev), 1;

	git_tree *tree;
	switch (git_object_type(obj)) {
		case GIT_OBJ_COMMIT:
			git_oid_fmt(sha, git_commit_id((git_commit*)obj));
			sha[GIT_OID_HEXSZ] = '\0';
			debug("using commit %s\n", sha);

			/* rev points to a commit, lookup corresponding
			 * tree */
			if (git_commit_tree(&tree, (git_commit*)obj) < 0) {
				return error("Failed to lookup tree for rev: %s\n", rev), 1;
			}
			gitfs_commit_time = git_commit_time((git_commit*)obj);
			git_object_free(obj);
			break;
		case GIT_OBJ_TREE:
			/* rev points to a tree, just use it */
			tree = (git_tree*)obj;

			/* Trees don't store any time information, so
			 * just use the current time (better than using
			 * 0, which can confuse programs such as tar).
			 * */
			gitfs_commit_time = time(NULL);
			break;
		default:
			return error("rev does not point to a tree or commit: %s\n", rev), 1;
	}

	git_oid_fmt(sha, git_tree_id(tree));
	sha[GIT_OID_HEXSZ] = '\0';
	debug("using tree %s\n", sha);

	/* Save the oid we found, for gitfs_init to open after chrooting */
	git_oid_cpy(&gitfs_tree_oid, git_tree_id(tree));

	/* Unallocate this stuff, since it's useless after chrooting */
	git_tree_free(tree);
	git_repository_free(repo);

	char *opts = NULL; /* fuse_opt_add_opt will allocate this */

	/* Force the mount to be read-only */
	fuse_opt_add_opt(&opts, "ro");

	/* Set a meaningful fsname (e.g., to let mount show
	 * "foo.git mounted on /somewhere"). */
	char fsname_opt[PATH_MAX + 8];
	snprintf(fsname_opt, lengthof(fsname_opt), "fsname=%s", gitfs_repo_path);
	fuse_opt_add_opt_escaped(&opts, fsname_opt);

	/* Make the filsystem type "fuse.git-fs" (this is the default if
	 * fsname is not specified). */
	fuse_opt_add_opt(&opts, "subtype=git-fs");

	/* Since we are a read-only filesystem and our contents cannot
	 * be externally modified (note that even if the git repository
	 * changes, the specific tree object we've locked onto can never
	 * change!), enable some aggresive caching to greatly improve
	 * performance. */

	/* This enables the usual kernel caching methods for file
	 * contents. The kernel normally takes care of updating any
	 * cache entries when they are written to (so this only works
	 * when the filesystem is only written to through fuse). */
	fuse_opt_add_opt(&opts, "kernel_cache");

	/* These enable more aggresive caching of file existence and
	 * attributes (the default is 1 second, but since our contents
	 * never change, we raise this to 600 seconds). */
	fuse_opt_add_opt(&opts, "entry_timeout=600");
	fuse_opt_add_opt(&opts, "negative_timeout=600");
	fuse_opt_add_opt(&opts, "attr_timeout=600");

	/* Tell the kernel to go ahead and check permissions based on
	 * the mode we return in getattr (by default, we're supposed to
	 * check permissions in open, getattr, etc. but we don't want
	 * that). There's probably not much permissions to check, but
	 * let's set this anyway. */
	fuse_opt_add_opt(&opts, "default_permissions");

	/* Force fuse to use single-threaded mode, since libgit2 is not
	 * yet thread-safe. */
	fuse_opt_insert_arg(&args, 1, "-s");

	/* Append the options collected in opts */
	fuse_opt_insert_arg(&args, 1, "-o");
	fuse_opt_insert_arg(&args, 2, opts);

	free(opts);
	opts = NULL;

	/* Allow git_init to change our exit code */
	retval = 0;
	fuse_main(args.argc, args.argv, &gitfs_oper);
	return retval;
}

