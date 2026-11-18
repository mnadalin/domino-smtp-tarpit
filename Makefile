DOMINO_DIR ?= /opt/hcl/domino/notes/14050100/linux
CAPI_INC   ?= /opt/hcl/CAPIToolkit/include

CC      = gcc
CFLAGS  = -shared -fPIC -O2 -Wall -pthread \
          -I$(CAPI_INC) \
          -DGCC4 \
          -DUNIX -DND64 -DNDUNIX \
          -DLINUX -DLINUX64 -DLONGIS64BIT \
          -DPRODUCTION_VERSION -DOVERRIDEDEBUG
LDFLAGS = -L$(DOMINO_DIR) -lnotes \
          -Wl,-rpath,$(DOMINO_DIR)

TARGET  = libsmtp-tarpit.so
SRC     = smtp-tarpit.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)

# Used by build.sh to deploy into the same directory the library links against.
print-domino-dir:
	@echo $(DOMINO_DIR)

.PHONY: all clean print-domino-dir
