# usb-floppy

Command-line tool for Linux and macOS to initialize, mount, and manage raw virtual floppy slots on USB sticks for stock 7-segment Gotek floppy emulators (such as those used with Yamaha PSR keyboards).

## Build
```bash
clang++ -std=c++20 -O2 floppy_usb.cpp -o floppy_usb
```

## Usage
1. Format Slot `000` directly on your keyboard/hardware once.
2. Clone that exact format across 100 (or 1,000) slots:
   ```bash
   sudo ./floppy_usb init 100
   ```
3. Mount a range of slots in Finder:
   ```bash
   sudo ./floppy_usb mount 0-9
   ```
4. Copy your files in Finder or Terminal, then clean macOS metadata and sync changes back to the USB stick:
   ```bash
   sudo ./floppy_usb unmount
   ```
