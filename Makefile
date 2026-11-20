CXX      ?= c++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra
TARGET   := floppy_usb
SRC      := floppy_usb.cpp
PREFIX   ?= /usr/local/bin

.PHONY: all install uninstall clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)
	install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/$(TARGET)

clean:
	rm -f $(TARGET)
