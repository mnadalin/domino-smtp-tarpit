DOMINO_DIR ?= /opt/hcl/domino/notes/14050000/linux
CAPI_INC   ?= /opt/hcl/CAPIToolkit/include

CC      = gcc
CFLAGS  = -shared -fPIC -O2 -Wall \
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
