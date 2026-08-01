CC := gcc
CPPFLAGS := -D_POSIX_C_SOURCE=200809L
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Werror
LDFLAGS :=
LDLIBS :=

PROGRAMS := diskinfo disklist diskget diskput
COMMON_OBJECT := shared.o

.PHONY: all clean rebuild

all: $(PROGRAMS)

diskinfo: diskinfo.o $(COMMON_OBJECT)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

disklist: disklist.o $(COMMON_OBJECT)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

diskget: diskget.o $(COMMON_OBJECT)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

diskput: diskput.o $(COMMON_OBJECT)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c shared.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(PROGRAMS) *.o

rebuild: clean all