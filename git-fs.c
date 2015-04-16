/*
 * fuse + libgit2 = read-only mounting of bare repos
 */

#define FUSE_USE_VERSION 26
#define _FILE_OFFSET_BITS 64
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <git2.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <execinfo.h>

/* http://pubs.opengroup.org/onlinepubs/009695399/basedefs/limits.h.html
 */
#ifndef PATH_MAX
	#include <linux/limits.h>
#endif

/* Macro to get the length of a static array */
#define lengthof(arr) (sizeof(arr) / sizeof(*arr))

bool enable_debug = 0;
int error_fd = 2;

#define error(...) dprintf(error_fd, __VA_ARGS__)

// Dump a stacktrace to stderr
static void dump_trace(int signum) {
	error("Segmentation fault\n");
	void * buffer[255];
	const int calls = backtrace(buffer, lengthof(buffer));
	if (calls == 0) {
		error("Failed to get a backtrace");
	} else {
		// print trace to stderr
		backtrace_symbols_fd(buffer, calls, error_fd);
	}
	exit(1);
}

/* Macro to hide the ugly casts needed to access fi->fh (which is a
 * uint64_t, which can store pointers, but is too big on 32-bit systems,
 * requiring multiple casts to silence the compiler warnings). */
#define GITFS_FH(fi) ((gitfs_entry *)(intptr_t)fi->fh)

typedef enum {
	GITFS_FILE,
	GITFS_DIR,
	/* A special (virtual) file that contains an object id (hash). */
	GITFS_OID,
} gitfs_entry_type;

typedef struct gitfs_entry {
	/** The type */
	gitfs_entry_type type;
	/* The tree_entry for this entry, when type is GITFS_FILE. */
	git_tree_entry *tree_entry;
	/* The tree, blob or oid (in string form) corresponding to this
	 * entry */
	union {
		git_tree *tree;
		git_blob *blob;
		/* Content of the file (hash in ascii form).
		 * Must be exactly GIT_OID_HEXSZ + 1 characters
		 * long, contain a trailing newline but no
		 * nul-termination. */
		char *oid;
	} object;
} gitfs_entry;

struct gitfs_data {
	/* Options passed on the cmdline */
	char *repo_path;
	char *rev;
	bool no_oid_files;

	/* Mounted commit / tree */
	time_t commit_time;
	git_oid tree_oid;

	git_repository *repo;
	git_tree *tree;

	/* Allocate for up to two oid files (but there might be less */
	gitfs_entry oid_entries[2];
	/* Paths corresponding to each entry in oid_entries. Should each
	 * be a leading slash followed by a plain filename (no
	 * subdirectories allowed) */
	const char *oid_paths[2];
	/* The number of valid entries in oid_entries */
	size_t oid_entry_count;

	/* Value to return when fuse_main exits */
	int retval;

};

void debug(const char* format, ...) {
	if (!enable_debug)
		return;
	va_list args;
	va_start(args,format);
	vfprintf(stderr, format, args);
	va_end(args);
}


void gitfs_entry_free(gitfs_entry *e) {
	struct gitfs_data *d = (struct gitfs_data *)(fuse_get_context()->private_data);
	switch (e->type) {
		case GITFS_DIR:
			if (e->object.tree != d->tree)
				git_tree_free(e->object.tree);
			break;
		case GITFS_FILE:
			git_tree_entry_free(e->tree_entry);
			git_blob_free(e->object.blob);
			break;
		case GITFS_OID:
			/* Don't free GITFS_OID entries, they're statically
			 * allocated in gitfs_data. The contents stored in them
			 * will be explicitely freed by gitfs_destroy. */
			return;
	}

	free(e);
}

int gitfs_lookup_oid_entry(gitfs_entry **out, const char *path) {
	struct gitfs_data *d = (struct gitfs_data *)(fuse_get_context()->private_data);
	int i;
	for (i = 0; i < d->oid_entry_count; i++) {
		if (!strcmp(path, d->oid_paths[i])) {
			*out = &d->oid_entries[i];
			return 0;
		}
	}
	return -ENOENT;
}

int gitfs_lookup_git_entry(gitfs_entry **out, const char *path) {
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
		e->type = GITFS_DIR;
		e->object.tree = d->tree;
		return 0;
	}

	git_tree_entry *tree_entry = NULL;
	/* Fill e->tree_entry */
	if (git_tree_entry_bypath(&tree_entry, d->tree, path + 1) < 0) {
		retval = -ENOENT;
		goto out;
	}

	/* Fill e->type */
	switch(git_tree_entry_type(tree_entry)) {
		case GIT_OBJ_TREE:
			/* Lookup the corresponding git_tree object and
			 * store it into e->object */
			if (git_tree_entry_to_object((git_object**)&e->object.tree, d->repo, tree_entry) < 0) {
				error("Tree not found?!: '%s'\n", path);
				retval = -EIO;
				goto out;
			}
			e->type = GITFS_DIR;
			break;

		case GIT_OBJ_BLOB:
			/* Lookup the corresponding git_blob object and
			 * store it into e->object */
			if (git_tree_entry_to_object((git_object**)&e->object.blob, d->repo, tree_entry) < 0) {
				error("Blob not found?!: '%s'\n", path);
				retval = -EIO;
				goto out;
			}
			e->type = GITFS_FILE;
			e->tree_entry = tree_entry;
			tree_entry = NULL;
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
	git_tree_entry_free(tree_entry);

	return retval;
}

int gitfs_lookup_entry(gitfs_entry **out, const char *path) {
	int retval = gitfs_lookup_git_entry(out, path);

	/* Path not found in git, see if it's one of the magic oid paths */
	if (retval == -ENOENT)
		retval = gitfs_lookup_oid_entry(out, path);

	if (retval == -ENOENT)
		debug("File not found: '%s'\n", path);

	return retval;
}

/**
 * Initialize an oid entry, which is a magic file inside / that contains
 * an oid. Path must be the pathname, including leading /. The pointer
 * in path must not be freed until gitfs_destroy, since it is used as is
 * (e.g., a string constant is perfect).
 */
int gitfs_init_oid_entry(struct gitfs_data *d, const char *path, const git_oid* oid)
{
	/* Disabled, skip */
	if (d->no_oid_files)
		return 0;

	/* Check if the statically allocated oid_entries array is long
	 * enough. This is a sanity check, this can only occur when the
	 * code is (incorrectly) modified. */
	if (d->oid_entry_count == lengthof(d->oid_entries))
		return error("oid_entries is nog long enough?!\n"), -ENOMEM;

	/* Copy the path (pointer) */
	d->oid_paths[d->oid_entry_count] = path;

	/* Fill the entry */
	gitfs_entry *e = &d->oid_entries[d->oid_entry_count];
	e->tree_entry = NULL;
	e->type = GITFS_OID;

	e->object.oid = malloc(GIT_OID_HEXSZ + 1);
	if (!e->object.oid)
		return error("Could not allocate memory for oid file contents (%s)\n", path), -ENOMEM;
	git_oid_fmt(e->object.oid, oid);
	e->object.oid[GIT_OID_HEXSZ] = '\n';

	d->oid_entry_count++;

	return 0;
}

int gitfs_open(const char *path, struct fuse_file_info *fi)
{
	/* Find the corresponding entry and store it inside the fh
	 * member, for use in other operations. */
	return gitfs_lookup_entry((gitfs_entry**)&fi->fh, path);
}

int gitfs_release(const char *path, struct fuse_file_info *fi)
{
	/* Free the gitfs_entry pointer in fh */
	if(fi->fh)
		gitfs_entry_free(GITFS_FH(fi));
	return 0;
}

int gitfs_getattr(const char *path, struct stat *stbuf)
{
	struct gitfs_data *d = (struct gitfs_data *)(fuse_get_context()->private_data);
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
	stbuf->st_atime = d->commit_time;
	stbuf->st_ctime = d->commit_time;
	stbuf->st_mtime = d->commit_time;

	if (e->type == GITFS_DIR) {
		debug( "Path is a directory: '%s'\n", path);
		stbuf->st_nlink = 2;
		stbuf->st_mode = 040755;
		stbuf->st_size = 4096;
	} else if (e->type == GITFS_FILE) {
		debug( "Path is a file: '%s'\n", path);
		stbuf->st_nlink = 1;
		stbuf->st_mode = git_tree_entry_filemode(e->tree_entry);
		/* Override the permissions for links, since git just
		 * stores the link type bit. */
		if (S_ISLNK(stbuf->st_mode))
			stbuf->st_mode = S_IFLNK | S_IRWXU | S_IRWXG | S_IRWXO;
		/* Note that this gives the length of the filename for
		 * symlinks, but that's what native filesystems do as
		 * well. */
		stbuf->st_size = git_blob_rawsize(e->object.blob);
	} else if (e->type == GITFS_OID) {
		debug( "Path is a special oid file: '%s'\n", path);
		stbuf->st_nlink = 1;
		/* Read-only for everyone */
		stbuf->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
		stbuf->st_size = GIT_OID_HEXSZ + 1;
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
	struct gitfs_data *d = (struct gitfs_data *)(fuse_get_context()->private_data);
	debug("readdir called for '%s'\n", path);
	gitfs_entry *e = GITFS_FH(fi);
	if (e->type != GITFS_DIR)
		return debug("Path is not a directory?!: '%s'\n", path), -EIO;

	int entry_count = git_tree_entrycount(e->object.tree);
	while (offset < (entry_count)) {
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

	if (path[0] == '/' && path[1] == '\0') {
		/* Dirlisting of root dir /, insert all magic oid paths
		 * first. */
		while (offset - entry_count < d->oid_entry_count) {
			/* Note that we skip the first char of
			 * object.oid.path, which is a leading / for
			 * easy comparison in gitfs_lookup_oid_entry. */
			if (filler(buf, d->oid_paths[offset - entry_count] + 1, NULL, offset + 1) == 1)
				return 0;
			offset++;
		}
	}


	return 0;
}

int gitfs_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	debug("read called for '%s' (offset %d, size %d)\n", path, offset, size);
	size_t blob_size;
	const void *blob;

	gitfs_entry *e = GITFS_FH(fi);
	debug("type %d\n", e->type);
	switch (e->type) {
		case GITFS_FILE:
			if (!S_ISREG(git_tree_entry_filemode(e->tree_entry)))
				return error("Path is not a regular file?!: '%s'\n", path), -EIO;
			blob_size = git_blob_rawsize(e->object.blob);
			blob = git_blob_rawcontent(e->object.blob);
			break;
		case GITFS_OID:
			blob_size = GIT_OID_HEXSZ + 1;
			blob = e->object.oid;
			break;
		default:
			return error("Path is not a file?!: '%s'\n", path), -EIO;
	}

	if (offset >= blob_size)
		size = 0;
	else if (offset + size > blob_size)
		size = blob_size - offset;

	if (size)
		memcpy(buf, blob + offset, size);

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

	if (e->type != GITFS_FILE || !S_ISLNK(git_tree_entry_filemode(e->tree_entry))) {
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
	int i;

	if (d) {
		if (d->tree) git_tree_free(d->tree);
		if (d->repo) git_repository_free(d->repo);
		for (i = 0; i < d->oid_entry_count; i++) {
			free(d->oid_entries[i].object.oid);
		}
	}
}

void* gitfs_init(struct fuse_conn_info *conn) {
	char sha[GIT_OID_HEXSZ + 1];
	/* Start by chrooting into the git repository. Doing this allows
	 * git-fs to be started from within initrd and not break if
	 * mount points are shuffled around, causing the location of the
	 * git repository to change. By chrooting into the .git dir,
	 * anything can happen, except for unmounting it completely.
	 * Note that we can't do this chroot in main(), since fuse_main
	 * needs /dev/fuse and possibly /dev/null and others too... */
	struct gitfs_data *d = (struct gitfs_data *)(fuse_get_context()->private_data);
	debug("chrooting to %s\n", d->repo_path);

	if (chroot(d->repo_path) < 0) {
		error("Failed to chroot to %s: %s\n", d->repo_path, strerror(errno));
		goto err;
	}
	if (chdir("/") < 0) {
		error("Failed to chdir to /: %s\n", strerror(errno));
		goto err;
	}

	debug("opening repo after fuse_main\n");
	if (git_repository_open(&d->repo, "/") < 0) {
		error("Cannot open git repository: %s\n", giterr_last()->message);
		goto err;
	}

	if (git_tree_lookup(&d->tree, d->repo, &d->tree_oid) < 0) {
		git_oid_fmt(sha, &d->tree_oid);
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
	d->retval = 1;
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
	     "    -o no-oid-files\n"
	     "        Don't export magic files /.git-fs-tree-id and\n"
	     "        (when applicable) /.git-fs-commit-id containing\n"
	     "        the hashes of the mounted tree and commit\n"
	     "        respectively.\n"
	     "\n"
	     , args->argv[0]);
             fuse_opt_add_arg(args, "-ho");
             fuse_main(args->argc, args->argv, &gitfs_oper, NULL);
}

enum {
	KEY_DEBUG,
	KEY_HELP,
	KEY_REV,
	KEY_RWRO,
	KEY_NO_OID_FILES,
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
	FUSE_OPT_KEY("no-oid-files",   KEY_NO_OID_FILES),
	FUSE_OPT_END
};

static int gitfs_opt_proc(void *data, const char *arg, int key, struct fuse_args *outargs)
{
	struct gitfs_data *d = (struct gitfs_data *)data;

	/* The first non-option argument is the repo path */
	if (key == FUSE_OPT_KEY_NONOPT && d->repo_path == NULL) {
		d->repo_path = realpath(arg, NULL);
		if (d->repo_path == NULL) {
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
		if (d->rev != NULL) {
			error("--rev / -o rev can be passed only once\n");
			return -1;
		}
		d->rev = strdup(strchr(arg, '=') + 1);
		/* Don't pass this option onto fuse_main */
		return 0;
	} else if (key == KEY_HELP) {
		usage(outargs, stdout);
		exit(0);
	} else if (key == KEY_NO_OID_FILES) {
		d->no_oid_files = 1;
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
	char sha[41];

	// Do a dummy backtrace call. This loads some files (ld.so.cache) and
	// ldopens libgcc_s.so, which are not available anymore later due to
	// the chroot (and might not be ideal to do in a signal handler anyway).
	void *dummy[1];
	backtrace(dummy, 1);

	// Dump a stack trace on a segfault
	signal(SIGSEGV, dump_trace);
	signal(SIGABRT, dump_trace);

	struct gitfs_data *d = calloc(1, sizeof(struct gitfs_data));
	if (!d) {
		return error("Failed to allocate memory for userdata\n"), 1;
	}

	if (fuse_opt_parse(&args, d, gitfs_opts, gitfs_opt_proc))
		return 1;

	if (d->repo_path == NULL)
		return error("No repository path given\n\n"), usage(&args, stderr), 1;

	if (stat(d->repo_path, &st) < 0 || !S_ISDIR(st.st_mode))
		return error("%s: path does not exist?\n", d->repo_path), 1;

	/* We open the repo now and resolve the arguments given, so we
	 * can bail out and provide an error message when anything is
	 * wrong. We'll have to re-open the repository later in
	 * gitfs_init after the chroot, since the chroot will break the
	 * repository object (but once we are there, we might have
	 * already detached from the terminal, so it's too late to
	 * provide useful error messages). */
	debug("opening repo before fuse_main\n");
	git_repository *repo;
	if (git_repository_open(&repo, d->repo_path) < 0)
		return error("Cannot open git repository: %s\n", giterr_last()->message), 1;

	/* Default to HEAD */
	const char *rev = "HEAD";
	if (d->rev)
		rev = d->rev;
	debug("using rev %s\n", rev);

	git_object *obj;
	if (git_revparse_single(&obj, repo, rev) < 0)
		return error("Failed to resolve rev: %s\n", rev), 1;

	git_tree *tree;
	git_commit *commit;
	switch (git_object_type(obj)) {
		case GIT_OBJ_COMMIT:
			commit = (git_commit*)obj;
			git_oid_fmt(sha, git_commit_id(commit));
			sha[GIT_OID_HEXSZ] = '\0';
			debug("using commit %s\n", sha);

			/* rev points to a commit, lookup corresponding
			 * tree */
			if (git_commit_tree(&tree, commit) < 0) {
				return error("Failed to lookup tree for rev: %s\n", rev), 1;
			}
			d->commit_time = git_commit_time(commit);

			/* Export the commit id through a magic file */
			if (gitfs_init_oid_entry(d, "/.git-fs-commit-id", git_commit_id(commit)) < 0)
				return 1;
			git_object_free(obj);
			break;
		case GIT_OBJ_TREE:
			/* rev points to a tree, just use it */
			tree = (git_tree*)obj;

			git_oid_fmt(sha, git_tree_id(tree));
			sha[GIT_OID_HEXSZ] = '\0';
			debug("using commit %s\n", sha);

			/* Trees don't store any time information, so
			 * just use the current time (better than using
			 * 0, which can confuse programs such as tar).
			 * */
			d->commit_time = time(NULL);
			break;
		default:
			return error("rev does not point to a tree or commit: %s\n", rev), 1;
	}

	git_oid_fmt(sha, git_tree_id(tree));
	sha[GIT_OID_HEXSZ] = '\0';
	debug("using tree %s\n", sha);

	/* Save the oid we found, for gitfs_init to open after chrooting */
	git_oid_cpy(&d->tree_oid, git_tree_id(tree));

	/* Export the tree id through a magic file */
	if (gitfs_init_oid_entry(d, "/.git-fs-tree-id", &d->tree_oid) < 0)
		return 1;


	/* Unallocate this stuff, since it's useless after chrooting */
	git_tree_free(tree);
	git_repository_free(repo);

	char *opts = NULL; /* fuse_opt_add_opt will allocate this */

	/* Force the mount to be read-only */
	fuse_opt_add_opt(&opts, "ro");

	/* Set a meaningful fsname (e.g., to let mount show
	 * "foo.git mounted on /somewhere"). */
	char fsname_opt[PATH_MAX + 8];
	snprintf(fsname_opt, lengthof(fsname_opt), "fsname=%s", d->repo_path);
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

	/* Append the options collected in opts */
	fuse_opt_insert_arg(&args, 1, "-o");
	fuse_opt_insert_arg(&args, 2, opts);

	free(opts);
	opts = NULL;

	/* fuse_main will redirect stderr to /dev/null, so keep a reference
	 * around in case we need to print a segfault trace */
	error_fd = dup(error_fd);

	/* Pass d as user_data, which will be made available through the
	 * context in gitfs_init. */
	fuse_main(args.argc, args.argv, &gitfs_oper, d);

	fuse_opt_free_args(&args);

	free(d->repo_path);
	free(d->rev);
	free(d);

	/* Allow git_init to change our exit code */
	return d->retval;
}

