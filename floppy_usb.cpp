#include <iostream>
#include <vector>
#include <array>
#include <string>
#include <format>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/disk.h>
#include <cstdlib>

namespace fs = std::filesystem;

constexpr uint64_t FLOPPY_BYTES      = 1474560ULL; // 1.44 MB actual FAT12 image size
constexpr uint64_t SLOT_STRIDE_BYTES = 1572864ULL; // 1.5 MiB (3072 sectors) Gotek hardware stride
constexpr uint64_t SECTOR_BYTES      = 512ULL;
const std::string WORK_DIR = "/tmp/yamaha_gotek_slots";
const std::string STATE_FILE = "/tmp/yamaha_gotek_slots/state.txt";

struct DiskInfo
{
    std::string blockPath; // e.g., /dev/disk5
    std::string rawPath;   // e.g., /dev/rdisk5
    uint64_t totalBytes = 0;
    uint32_t maxPossibleSlots = 0;
};

std::string execCommand(const std::string &cmd, int &exitCode)
{
    std::string result;
    std::array<char, 256> buffer{};
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe)
    {
        exitCode = -1;
        return "";
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
    {
        result += buffer.data();
    }
    exitCode = pclose(pipe);
    return result;
}

DiskInfo getDiskInfo(const std::string& diskIdentifier) {
    DiskInfo info;
    info.blockPath = "/dev/" + diskIdentifier;
    info.rawPath   = "/dev/r" + diskIdentifier;

    // Unmount any auto-mounted slice (like Slot 000) so the kernel exposes the full USB stick capacity
    std::string unmountCmd = std::format("diskutil unmountDisk {} >/dev/null 2>&1", info.blockPath);
    std::system(unmountCmd.c_str());

    int fd = open(info.rawPath.c_str(), O_RDONLY);
    if (fd < 0) {
        perror("open raw disk failed (run with sudo)");
        return info;
    }

    uint64_t blockCount = 0;
    uint32_t blockSize  = 0;
    if (ioctl(fd, DKIOCGETBLOCKCOUNT, &blockCount) == 0 &&
        ioctl(fd, DKIOCGETBLOCKSIZE, &blockSize) == 0) {
        info.totalBytes = blockCount * static_cast<uint64_t>(blockSize);
        info.maxPossibleSlots = static_cast<uint32_t>(info.totalBytes / SLOT_STRIDE_BYTES);
    }
    close(fd);
    return info;
}

std::vector<uint32_t> scanExistingSlots(const DiskInfo& disk, uint32_t startSlot, uint32_t endSlot) {
    std::vector<uint32_t> validSlots;
    if (disk.maxPossibleSlots == 0 || startSlot >= disk.maxPossibleSlots) {
        return validSlots;
    }

    int fd = open(disk.rawPath.c_str(), O_RDONLY);
    if (fd < 0) {
        perror("open raw disk failed during scan");
        return validSlots;
    }

    uint32_t maxIndex   = disk.maxPossibleSlots - 1;
    uint32_t clampedEnd = std::min(endSlot, maxIndex);
    std::array<uint8_t, 512> bootSector{};

    for (uint32_t slot = startSlot; slot <= clampedEnd; ++slot) {
        off_t offset = static_cast<off_t>(slot) * SLOT_STRIDE_BYTES;
        ssize_t n = pread(fd, bootSector.data(), bootSector.size(), offset);
        if (n < 512) break;

        bool hasBootSig = (bootSector.at(510) == 0x55 && bootSector.at(511) == 0xAA);
        bool isFat12    = (std::memcmp(bootSector.data() + 54, "FAT12", 5) == 0);

        if (hasBootSig && isFat12) {
            validSlots.push_back(slot);
        } else {
            break;
        }
    }

    close(fd);
    return validSlots;
}

// Patch 11-byte FAT12 volume label AND 4-byte unique serial number so macOS mounts all slots
void customizeFat12Slot(std::vector<uint8_t>& img, uint32_t slotIndex) {
    // 1. Unique 32-bit Volume Serial Number at offset 39 (0x27) so diskarbitrationd mounts every slot
    uint32_t serial = 0x59414D00u + slotIndex; // "YAM\0" + slotIndex
    std::memcpy(img.data() + 39, &serial, sizeof(serial));

    // 2. 11-byte Volume Label at offset 43 (0x2B) in Boot Sector
    std::array<char, 12> label{};
    std::snprintf(label.data(), label.size(), "SLOT_%03u   ", slotIndex % 1000);
    std::memcpy(img.data() + 43, label.data(), 11);

    // 3. 11-byte Volume Label in Root Directory (sector 19 = byte 9728)
    constexpr size_t ROOT_DIR_OFFSET = 19 * 512;
    std::memcpy(img.data() + ROOT_DIR_OFFSET, label.data(), 11);
    img.at(ROOT_DIR_OFFSET + 11) = 0x08;
}

// Patch ONLY the temporary /tmp .img file so macOS mounts multiple slots simultaneously
void patchTempImageForMacOS(std::vector<uint8_t>& img, uint32_t slotIndex) {
    uint32_t serial = 0x59414D00u + slotIndex;
    std::memcpy(img.data() + 39, &serial, sizeof(serial));

    std::array<char, 12> label{};
    std::snprintf(label.data(), label.size(), "SLOT_%03u   ", slotIndex % 1000);
    std::memcpy(img.data() + 43, label.data(), 11);
}

void mountAllSlots(const DiskInfo& disk, const std::vector<uint32_t>& slots) {
    if (fs::exists(STATE_FILE)) {
        std::cerr << "Slots are already mounted. Run 'sudo ./floppy_usb unmount' first.\n";
        return;
    }

    fs::create_directories(WORK_DIR);

    int fd = open(disk.rawPath.c_str(), O_RDONLY);
    if (fd < 0) {
        perror("open raw disk failed");
        return;
    }

    std::ofstream stateOut(STATE_FILE);
    stateOut << disk.rawPath << "\n";

    std::vector<uint8_t> slotBuffer(FLOPPY_BYTES);

    for (uint32_t slot : slots) {
        off_t offset = static_cast<off_t>(slot) * SLOT_STRIDE_BYTES;
        if (pread(fd, slotBuffer.data(), FLOPPY_BYTES, offset) != static_cast<ssize_t>(FLOPPY_BYTES)) {
            std::cerr << std::format("Failed reading Slot {:03d}\n", slot);
            break;
        }

        // Save original Yamaha 512-byte boot sector so we can restore it on unmount
        std::string bootBackupPath = std::format("{}/BOOT_{:03d}.bin", WORK_DIR, slot);
        {
            std::ofstream bootOut(bootBackupPath, std::ios::binary);
            bootOut.write(reinterpret_cast<const char*>(slotBuffer.data()), 512);
        }

        // Patch ONLY the temporary .img so macOS gives each mounted volume a unique ID & SLOT_XXX name
        patchTempImageForMacOS(slotBuffer, slot);

        std::string imgPath = std::format("{}/SLOT_{:03d}.img", WORK_DIR, slot);
        {
            std::ofstream out(imgPath, std::ios::binary);
            out.write(reinterpret_cast<const char*>(slotBuffer.data()), slotBuffer.size());
        }

        std::string attachCmd = std::format("hdiutil attach -noverify {} 2>&1", imgPath);
        int rc = 0;
        std::string output = execCommand(attachCmd, rc);

        if (rc == 0) {
            std::istringstream iss(output);
            std::string line;
            while (std::getline(iss, line)) {
                auto volPos = line.find("/Volumes/");
                if (volPos != std::string::npos) {
                    std::string devNode = line.substr(0, line.find_first_of(" \t"));
                    std::string volPath = line.substr(volPos);
                    stateOut << slot << "\t" << devNode << "\t" << volPath << "\t" << imgPath << "\n";
                    std::cout << std::format("Mounted Slot {:03d} -> {}\n", slot, volPath);
                }
            }
        } else {
            std::cerr << std::format("Failed to mount Slot {:03d}:\n{}\n", slot, output);
            break;
        }
    }
    close(fd);
}

void cleanHiddenFiles(const fs::path &volPath)
{
    std::error_code ec;
    if (!fs::exists(volPath, ec))
        return;

    for (const auto &item : fs::directory_iterator(volPath, ec))
    {
        std::string fname = item.path().filename().string();
        if (fname.starts_with("."))
        {
            fs::remove_all(item.path(), ec);
        }
    }
}

void unmountAndCleanAllSlots() {
    std::ifstream stateIn(STATE_FILE);
    if (!stateIn.is_open()) {
        std::cout << "No active mounted slots found.\n";
        return;
    }

    std::string rawDiskPath;
    if (!std::getline(stateIn, rawDiskPath) || rawDiskPath.empty()) {
        std::cerr << "Corrupt state file.\n";
        return;
    }

    std::string blockDiskPath = rawDiskPath;
    auto rPos = blockDiskPath.find("/rdisk");
    if (rPos != std::string::npos) {
        blockDiskPath.replace(rPos, 6, "/disk");
    }

    std::string unmountCmd = std::format("diskutil unmountDisk {} >/dev/null 2>&1", blockDiskPath);
    std::system(unmountCmd.c_str());

    int fd = open(rawDiskPath.c_str(), O_WRONLY);
    if (fd < 0) {
        perror("open raw disk for write-back failed (is USB stick still plugged in?)");
        return;
    }

    std::string line;
    std::vector<uint8_t> slotBuffer(FLOPPY_BYTES);

    while (std::getline(stateIn, line)) {
        std::istringstream iss(line);
        uint32_t slot = 0;
        std::string devNode, volPath, imgPath;

        if (!(iss >> slot >> devNode)) continue;
        std::getline(iss >> std::ws, line);
        auto tabPos = line.rfind('\t');
        if (tabPos == std::string::npos) continue;
        volPath = line.substr(0, tabPos);
        imgPath = line.substr(tabPos + 1);

        cleanHiddenFiles(volPath);

        std::string detachCmd = std::format("hdiutil detach {} >/dev/null 2>&1", devNode);
        if (std::system(detachCmd.c_str()) != 0) {
            std::cerr << std::format("Warning: could not detach {} ({})\n", volPath, devNode);
            continue;
        }

        std::ifstream in(imgPath, std::ios::binary);
        in.read(reinterpret_cast<char*>(slotBuffer.data()), slotBuffer.size());
        in.close();

        // Restore the exact original Yamaha 512-byte boot sector before writing back to USB
        std::string bootBackupPath = std::format("{}/BOOT_{:03d}.bin", WORK_DIR, slot);
        if (fs::exists(bootBackupPath)) {
            std::ifstream bootIn(bootBackupPath, std::ios::binary);
            bootIn.read(reinterpret_cast<char*>(slotBuffer.data()), 512);
        }

        off_t offset = static_cast<off_t>(slot) * SLOT_STRIDE_BYTES;
        if (pwrite(fd, slotBuffer.data(), FLOPPY_BYTES, offset) == static_cast<ssize_t>(FLOPPY_BYTES)) {
            std::cout << std::format("Saved & detached Slot {:03d} ({})\n", slot, volPath);
        } else {
            perror("pwrite failed");
        }
    }

    close(fd);
    stateIn.close();
    fs::remove_all(WORK_DIR);
    std::cout << "All slots synced to USB stick. You can safely unplug it now.\n";
}

bool initUsbStick(const DiskInfo& disk, uint32_t firmwareCap = 100) {
    uint32_t count = std::min(disk.maxPossibleSlots, firmwareCap);
    if (count == 0) return false;

    std::string unmountCmd = std::format("diskutil unmountDisk {} >/dev/null 2>&1", disk.blockPath);
    std::system(unmountCmd.c_str());

    int fd = open(disk.rawPath.c_str(), O_RDWR);
    if (fd < 0) {
        perror("open raw disk for init failed");
        return false;
    }

    // Read Slot 000 (formatted on the Yamaha PSR-1100) as our exact golden template
    std::vector<uint8_t> templateData(FLOPPY_BYTES);
    if (pread(fd, templateData.data(), FLOPPY_BYTES, 0) != static_cast<ssize_t>(FLOPPY_BYTES)) {
        perror("failed reading Slot 000 template");
        close(fd);
        return false;
    }

    if (templateData.at(510) != 0x55 || templateData.at(511) != 0xAA) {
        std::cerr << "Error: Slot 000 is not formatted yet! Format Slot 000 on the PSR-1100 first.\n";
        close(fd);
        return false;
    }

    // Clone Yamaha's EXACT bytes with zero modifications so the PSR-1100 accepts every slot
    for (uint32_t i = 0; i < count; ++i) {
        off_t offset = static_cast<off_t>(i) * SLOT_STRIDE_BYTES;
        if (pwrite(fd, templateData.data(), FLOPPY_BYTES, offset) != static_cast<ssize_t>(FLOPPY_BYTES)) {
            perror("pwrite failed");
            close(fd);
            return false;
        }
    }

    close(fd);
    std::cout << std::format("Cloned exact Yamaha format to {} slots on {}\n", count, disk.blockPath);
    return true;
}

struct SlotRange {
    uint32_t start = 0;
    uint32_t end   = 999; // inclusive
};

SlotRange parseRangeArg(const std::string& arg) {
    SlotRange range;
    auto dashPos = arg.find('-');
    if (dashPos != std::string::npos) {
        std::string left  = arg.substr(0, dashPos);
        std::string right = arg.substr(dashPos + 1);
        range.start = left.empty() ? 0 : static_cast<uint32_t>(std::stoul(left));
        range.end   = right.empty() ? 999 : static_cast<uint32_t>(std::stoul(right));
        if (range.end < range.start) std::swap(range.start, range.end);
    } else {
        uint32_t single = static_cast<uint32_t>(std::stoul(arg));
        range.start = single;
        range.end   = single;
    }
    return range;
}

std::string detectExternalUsbDisk() {
    int rc = 0;
    std::string out = execCommand("diskutil list external physical | grep '/dev/disk' | awk '{print $1}'", rc);
    std::istringstream iss(out);
    std::string firstDisk;
    if (std::getline(iss, firstDisk) && !firstDisk.empty()) {
        auto pos = firstDisk.find("/dev/");
        if (pos != std::string::npos) {
            return firstDisk.substr(pos + 5);
        }
    }
    return "";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage:\n"
                  << "  sudo ./floppy_usb mount [diskX] [slot | start-end]\n"
                  << "  sudo ./floppy_usb unmount\n"
                  << "  sudo ./floppy_usb init [diskX] [100|1000]\n";
        return 1;
    }

    std::string cmd = *(argv + 1);
    if (cmd == "unmount") {
        unmountAndCleanAllSlots();
        return 0;
    }

    if (cmd != "mount" && cmd != "init") {
        std::cerr << "Unknown command: " << cmd << "\n";
        return 1;
    }

    // Parse optional <diskX> and optional <range | count> arguments
    std::string diskId;
    std::string extraArg;

    if (argc >= 3) {
        std::string arg2 = *(argv + 2);
        if (arg2.starts_with("disk")) {
            diskId = arg2;
            if (argc >= 4) extraArg = *(argv + 3);
        } else {
            extraArg = arg2;
        }
    }

    if (diskId.empty()) {
        diskId = detectExternalUsbDisk();
        if (diskId.empty()) {
            std::cerr << "No external USB stick detected. Plug it in and try again.\n";
            return 1;
        }
        std::cout << "Auto-detected USB stick: " << diskId << "\n";
    }

    DiskInfo disk = getDiskInfo(diskId);
    if (disk.totalBytes == 0) return 1;

    try {
        if (cmd == "mount") {
            SlotRange range{0, 999};
            if (!extraArg.empty()) {
                range = parseRangeArg(extraArg);
            }
            auto slots = scanExistingSlots(disk, range.start, range.end);
            std::cout << std::format("Found {} formatted slots (range {}..{}).\n",
                                     slots.size(), range.start, range.end);
            mountAllSlots(disk, slots);
        } else if (cmd == "init") {
            uint32_t cap = extraArg.empty() ? 100 : static_cast<uint32_t>(std::stoul(extraArg));
            initUsbStick(disk, cap);
        }
    } catch (const std::exception& e) {
        std::cerr << "Invalid numeric argument: " << e.what() << "\n";
        return 1;
    }

    return 0;
}