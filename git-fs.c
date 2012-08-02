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

char *gitfs_repo_path = NULL;
int enable_debug = 0;
int retval;

struct gitfs_data {
	git_repository *repo;
	git_tree *tree;
};

#define error(...) fprintf(stderr, __VA_ARGS__)

/* Macro to hide the ugly casts needed to access fi->fh (which is a
 * uint64_t, which can store pointers, but is too big on 32-bit systems,
 * requiring multiple casts to silence the compiler warnings). */
#define GITFS_FH(fi) ((gitfs_entry *)(intptr_t)fi->fh)

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
	/** The type, GIT_OBJ_TREE or GIT_OBJ_BLOB */
	git_otype type;
	/* The tree or blob corresponding to this entry */
	union {
		git_tree *tree;
		git_blob *blob;
	} object;
} gitfs_entry;

void gitfs_entry_free(gitfs_entry *e) {
	if (e->type == GIT_OBJ_TREE && e->tree_entry != NULL)
		/* Don't free the tree when tree_entry is NULL, since
		 * that's the root tree */
		git_tree_free(e->object.tree);
	else if (e->type == GIT_OBJ_BLOB)
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
		e->type = GIT_OBJ_TREE;
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
	e->type = git_tree_entry_type(e->tree_entry);

	/* Fill e->object */
	if (e->type == GIT_OBJ_TREE) {
		/* Lookup the corresponding git_tree object */
		if (git_tree_entry_to_object((git_object**)&e->object.tree, d->repo, e->tree_entry) < 0) {
			error("Tree not found?!: '%s'\n", path);
			retval = -EIO;
			goto out;
		}
	} else if (e->type == GIT_OBJ_BLOB) {
		/* Lookup the corresponding git_blob object */
		if (git_tree_entry_to_object((git_object**)&e->object.blob, d->repo, e->tree_entry) < 0) {
			error("Blob not found?!: '%s'\n", path);
			retval = -EIO;
			goto out;
		}
	} else if (e->type == GIT_OBJ_COMMIT) {
		debug("Ignoring submodule entry: '%s'\n", path);
		retval = -ENOENT;
		goto out;
	} else {
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

	if (e->type == GIT_OBJ_TREE) {
		debug( "Path is a directory: '%s'\n", path);
		stbuf->st_nlink = 2;
		stbuf->st_mode = 040755;
		stbuf->st_size = 4096;
	} else if (e->type == GIT_OBJ_BLOB) {
		debug( "Path is a file: '%s'\n", path);
		stbuf->st_nlink = 1;
		stbuf->st_mode = git_tree_entry_attributes(e->tree_entry);
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
	if (e->type != GIT_OBJ_TREE)
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
	debug("read called for '%s'\n", path);
	gitfs_entry *e = GITFS_FH(fi);
	if (e->type != GIT_OBJ_BLOB || !S_ISREG(git_tree_entry_attributes(e->tree_entry)))
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

	if (e->type != GIT_OBJ_BLOB || !S_ISLNK(git_tree_entry_attributes(e->tree_entry))) {
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

	debug("opening repo\n");
	if (git_repository_open(&d->repo, "/") < 0) {
		error("Cannot open git repository: %s\n", giterr_last()->message);
		goto err;
	}


	git_oid oid;
	git_commit *commit;

	if (git_reference_name_to_oid(&oid, d->repo, "HEAD") < 0) {
		error("Failed to resolve ref: HEAD\n");
		goto err;
	}

	if (git_commit_lookup(&commit, d->repo, &oid) < 0) {
		error("Failed to lookup commit\n");
		goto err;
	}

	if (git_commit_tree(&d->tree, commit) < 0) {
		error("Failed to lookup tree\n");
		goto err;
	}
	git_commit_free(commit);

	/* This return value can be accessed through
	 * fuse_get_context()->private_data */
	return (void*)d;

err:
	if (commit) git_commit_free(commit);
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
		return error("%s: path does not exist?\n", gitfs_repo_path), 1;

	/* Allow git_init to change our exit code */
	retval = 0;
	fuse_main(args.argc, args.argv, &gitfs_oper);
	return retval;
}

