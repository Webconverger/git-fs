OPTS=-O2 -Wall -lfuse -I/opt/libgit2/include -L/opt/libgit2/lib -lgit2

git-fs: clean
	gcc ${OPTS} -o git-fs git-fs.c

example: git-fs
	test -d test-mount || mkdir test-mount
	sudo umount ./test-mount || true
	./git-fs . ./test-mount

clean:
	rm -f git-fs
