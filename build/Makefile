all: shell

clean:
	rm -f shell

CPPFLAGS := -D_BSD_SOURCE -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE
CFLAGS := -Wall -Wextra -std=c99 -pedantic -Werror

shell: shell.c
	$(CC) $(CFLAGS) $(CPPFLAGS) shell.c -o shell

.PHONY: all clean
