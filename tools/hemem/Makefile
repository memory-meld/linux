TARGET = libhemem.so
OBJECTS = hemem.o pebs.o interpose.o log.o fifo.o spsc-ring.o timer.o
CFLAGS += -fPIC -std=gnu2x -Wall -Wextra -Werror -g -DALLOC_HEMEM -I. -Wno-unused-parameter -Wno-unused-variable
LDFLAGS += -Wl,-rpath,'$$ORIGIN' -L. -ldl -lsyscall_intercept -Wl,--version-script=version.map

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) -shared $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ -o $@

clean:
	$(RM) $(TARGET) $(OBJECTS)

