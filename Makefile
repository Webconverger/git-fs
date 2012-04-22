OPTS=-Wall -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=25 -lfuse -I/opt/libgit2/include -L/opt/libgit2/lib -lgit2

example: test-mount
	./git-fs . ./test-mount

git-fs: clean 
	gcc ${OPTS} -o git-fs  git-fs.c

test: clean
	gcc ${OPTS} -o test  test.c

clean:
	rm -f git-fs

test-mount:
	test -d test-mount || mkdir test-mount
