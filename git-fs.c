#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <git2.h>

git_repository *repo = NULL;
git_tree *tree = NULL;
git_index *gindex = NULL;

git_index_entry *git_index_entry_by_file(const char *path) {
	unsigned int i, ecount;
	path++; // we always get an initial /

	ecount = git_index_entrycount(gindex);
	for (i = 0; i < ecount; ++i) {
		git_index_entry *e = git_index_get(gindex, i);
		if (! strcmp(e->path, path) == 0)
			continue;
		return e;
	}
	return NULL;
}

int git_getattr(const char *path, struct stat *stbuf)
{
	git_index_entry *e;
	if ( (e = git_index_entry_by_file(path)) == NULL)
		return fprintf(stderr,"path no exist? %s\n", path),-ENOENT;

	memset(stbuf, 0, sizeof(struct stat));
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
	const git_tree_entry *e;
	//int i,ecount = 0;

	//if (! (e = git_tree_entry_byname(tree, path)) )
	//	return -ENOENT;

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	return 0;
	/*ecount = git_tree_entrycount(tree);
	for ( i=0; i<ecount; i++) {
		git_index_entry *e = git_index_get(index, i);
		filler(buf, e->path + 1, NULL, 0);
	}
	*/

	return 0;
}

int git_open(const char *path, struct fuse_file_info *fi)
{
	return 0;
}

int git_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	(void) fi;
	return 0;
	/*
	size_t len;

	size = 0;
	len = strlen(file);
	if (offset < len) {
		if (offset + size > len)
			size = len - offset;
		memcpy(buf, git_str + offset, size);
	}

	return size;
	*/
}

struct fuse_operations git_oper = {
	.getattr= git_getattr,
	.readdir= git_readdir,
	.open= git_open,
	.read= git_read,
	.readdir= git_readdir
};

int main(int argc, char *argv[])
{
	char *o,*r,*m;
	char *a[]= { argv[0],argv[1],argv[2] };
	git_oid oid;

	m = argv[2];
	//o = argv[2+1];
	r = argv[2+1];
	//printf("using repo %s, mnt %s, oid %s\n",r, m, o);
	printf("using repo %s, mnt %s, \n",r, m);

	if (git_repository_open(&repo, r) < 0)
		return perror("repo"), 1;

	//if ( git_oid_fromstrn(&oid, o, strlen(o)) < 0)
	//	return fprintf(stderr, "oid %s -> %s", o, strerror(errno)),1;

	/* this fails for some reason?
	 * if ( git_tree_lookup(&tree, repo, &oid) < 0)
		return perror("tree lookup"),1;
		*/
	git_repository_index(&gindex, repo);
	git_index_read(gindex);

	return fuse_main(3, a, &git_oper);
}

