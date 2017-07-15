TARGET = USBMicroReader

OBJECTS = main.o

STYLEFIX = astyle

PKGS = glib-2.0

CFLAGS = -std=gnu99 -Wall -funsigned-char `pkg-config --cflags $(PKGS)` -DSTATIC=static
LDFLAGS = `pkg-config --libs $(PKGS)` -lm -lrfftw -lfftw -lsndfile

ifdef DEBUG
	CFLAGS += -ggdb -O0
else
	CFLAGS += -O2 -Werror
endif

all: $(TARGET)

# link
$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET) $(LDFLAGS)
	
STYLEFIX_FILES = \
	$(wildcard ./*.h) \
	$(wildcard ./*.c)
	
stylefix:
	$(STYLEFIX) --options=none --lineend=windows --style=allman --indent=force-tab=4 --break-blocks --indent-switches --pad-oper --pad-header --unpad-paren $(STYLEFIX_FILES)
	
clean:
	-rm -f $(OBJECTS) $(TARGET)

