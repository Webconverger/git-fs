# -rdynamic to allow printing a backtrace on as segfault
OPTS=-rdynamic -O2 -Wall -lfuse -lgit2

git-fs: clean
	gcc ${OPTS} -o git-fs git-fs.c

example: git-fs
	test -d test-mount || mkdir test-mount
	sudo umount ./test-mount || true
	./git-fs . ./test-mount

clean:
	rm -f git-fs
