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
#include <cstdlib>

#ifdef __APPLE__
#include <sys/disk.h>
#elif defined(__linux__)
#include <linux/fs.h>
#else
#error "Unsupported operating system. Only macOS and Linux are supported."
#endif

namespace fs = std::filesystem;

constexpr uint64_t FLOPPY_BYTES = 1474560ULL;      // 1.44 MB actual FAT12 image
constexpr uint64_t SLOT_STRIDE_BYTES = 1572864ULL; // 1.5 MB (3072 sectors) Gotek partition stride

const std::string WORK_DIR = "/tmp/yamaha_gotek_slots";
const std::string STATE_FILE = "/tmp/yamaha_gotek_slots/state.txt";

#ifdef __linux__
const std::string LINUX_MNT_BASE = "/mnt/gotek_slots";
#endif

struct DiskInfo
{
    std::string blockPath; // macOS: /dev/disk5 | Linux: /dev/sdb
    std::string rawPath;   // macOS: /dev/rdisk5 | Linux: /dev/sdb
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

void unmountPhysicalDisk(const std::string &blockPath)
{
#ifdef __APPLE__
    std::string cmd = std::format("diskutil unmountDisk {} >/dev/null 2>&1", blockPath);
#elif defined(__linux__)
    std::string cmd = std::format("umount {}* >/dev/null 2>&1", blockPath);
#endif
    std::system(cmd.c_str());
}

DiskInfo getDiskInfo(const std::string &diskIdentifier)
{
    DiskInfo info;
    std::string cleanId = diskIdentifier;
    if (cleanId.starts_with("/dev/"))
    {
        cleanId = cleanId.substr(5);
    }

#ifdef __APPLE__
    info.blockPath = "/dev/" + cleanId;
    info.rawPath = "/dev/r" + cleanId;
#elif defined(__linux__)
    info.blockPath = "/dev/" + cleanId;
    info.rawPath = "/dev/" + cleanId;
#endif

    int fd = open(info.rawPath.c_str(), O_RDONLY);
    if (fd < 0)
    {
        perror("open disk failed (run with sudo)");
        return info;
    }

#ifdef __APPLE__
    uint64_t blockCount = 0;
    uint32_t blockSize = 0;
    if (ioctl(fd, DKIOCGETBLOCKCOUNT, &blockCount) == 0 &&
        ioctl(fd, DKIOCGETBLOCKSIZE, &blockSize) == 0)
    {
        info.totalBytes = blockCount * static_cast<uint64_t>(blockSize);
    }
#elif defined(__linux__)
    uint64_t bytes = 0;
    if (ioctl(fd, BLKGETSIZE64, &bytes) == 0)
    {
        info.totalBytes = bytes;
    }
#endif

    info.maxPossibleSlots = static_cast<uint32_t>(info.totalBytes / SLOT_STRIDE_BYTES);
    close(fd);
    return info;
}

std::vector<uint32_t> scanExistingSlots(const DiskInfo &disk, uint32_t startSlot, uint32_t endSlot)
{
    std::vector<uint32_t> validSlots;
    if (disk.maxPossibleSlots == 0)
        return validSlots;

    int fd = open(disk.rawPath.c_str(), O_RDONLY);
    if (fd < 0)
        return validSlots;

    uint32_t maxIndex = disk.maxPossibleSlots - 1;
    uint32_t clampedEnd = std::min(endSlot, maxIndex);
    std::array<uint8_t, 512> bootSector{};

    for (uint32_t slot = startSlot; slot <= clampedEnd; ++slot)
    {
        off_t offset = static_cast<off_t>(slot) * SLOT_STRIDE_BYTES;
        ssize_t n = pread(fd, bootSector.data(), bootSector.size(), offset);
        if (n < 512)
            break;

        bool hasBootSig = (bootSector.at(510) == 0x55 && bootSector.at(511) == 0xAA);
        bool isFat12 = (std::memcmp(bootSector.data() + 54, "FAT12", 5) == 0);

        if (hasBootSig && isFat12)
        {
            validSlots.push_back(slot);
        }
        else
        {
            break;
        }
    }
    close(fd);
    return validSlots;
}

void mountAllSlots(const DiskInfo &disk, const std::vector<uint32_t> &slots)
{
    if (slots.empty())
    {
        std::cerr << "No valid FAT12 slots found to mount.\n";
        return;
    }
    if (fs::exists(STATE_FILE))
    {
        std::cerr << "Slots are already mounted. Run 'sudo ./floppy_usb unmount' first.\n";
        return;
    }

    fs::create_directories(WORK_DIR);
#ifdef __linux__
    fs::create_directories(LINUX_MNT_BASE);
    const char *sudoUid = std::getenv("SUDO_UID");
    const char *sudoGid = std::getenv("SUDO_GID");
    std::string uidStr = sudoUid ? sudoUid : "0";
    std::string gidStr = sudoGid ? sudoGid : "0";
#endif

    unmountPhysicalDisk(disk.blockPath);

    int fd = open(disk.rawPath.c_str(), O_RDONLY);
    if (fd < 0)
    {
        perror("open disk failed");
        return;
    }

    std::ofstream stateOut(STATE_FILE);
    stateOut << disk.rawPath << "\n";

    std::vector<uint8_t> slotBuffer(FLOPPY_BYTES);

    for (uint32_t slot : slots)
    {
        off_t offset = static_cast<off_t>(slot) * SLOT_STRIDE_BYTES;
        if (pread(fd, slotBuffer.data(), FLOPPY_BYTES, offset) != static_cast<ssize_t>(FLOPPY_BYTES))
        {
            std::cerr << std::format("Failed reading Slot {:03d}\n", slot);
            break;
        }

        std::string imgPath = std::format("{}/SLOT_{:03d}.img", WORK_DIR, slot);
        {
            std::ofstream out(imgPath, std::ios::binary);
            out.write(reinterpret_cast<const char *>(slotBuffer.data()), slotBuffer.size());
        }

#ifdef __APPLE__
        std::string attachCmd = std::format("hdiutil attach -noverify {} 2>&1", imgPath);
        int rc = 0;
        std::string output = execCommand(attachCmd, rc);

        if (rc == 0)
        {
            std::istringstream iss(output);
            std::string line;
            while (std::getline(iss, line))
            {
                auto volPos = line.find("/Volumes/");
                if (volPos != std::string::npos)
                {
                    std::string devNode = line.substr(0, line.find_first_of(" \t"));
                    std::string volPath = line.substr(volPos);
                    stateOut << slot << "\t" << devNode << "\t" << volPath << "\t" << imgPath << "\n";
                    std::cout << std::format("Mounted Slot {:03d} -> {}\n", slot, volPath);
                }
            }
        }
        else
        {
            std::cerr << std::format("Failed to mount Slot {:03d}:\n{}\n", slot, output);
            break;
        }
#elif defined(__linux__)
        std::string volPath = std::format("{}/SLOT_{:03d}", LINUX_MNT_BASE, slot);
        fs::create_directories(volPath);
        std::string mountCmd = std::format(
            "mount -t vfat -o loop,rw,uid={},gid={},shortname=win95 {} {} 2>&1",
            uidStr, gidStr, imgPath, volPath);
        int rc = 0;
        std::string output = execCommand(mountCmd, rc);

        if (rc == 0)
        {
            stateOut << slot << "\tloop\t" << volPath << "\t" << imgPath << "\n";
            std::cout << std::format("Mounted Slot {:03d} -> {}\n", slot, volPath);
        }
        else
        {
            std::cerr << std::format("Failed to mount Slot {:03d}:\n{}\n", slot, output);
            break;
        }
#endif
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
        if (fname.starts_with(".") || fname == "System Volume Information")
        {
            fs::remove_all(item.path(), ec);
        }
    }
}

void unmountAndCleanAllSlots()
{
    std::ifstream stateIn(STATE_FILE);
    if (!stateIn.is_open())
    {
        std::cout << "No active mounted slots found.\n";
        return;
    }

    std::string rawDiskPath;
    if (!std::getline(stateIn, rawDiskPath) || rawDiskPath.empty())
    {
        std::cerr << "Corrupt state file.\n";
        return;
    }

    std::string blockDiskPath = rawDiskPath;
#ifdef __APPLE__
    auto rPos = blockDiskPath.find("/rdisk");
    if (rPos != std::string::npos)
    {
        blockDiskPath.replace(rPos, 6, "/disk");
    }
#endif

    unmountPhysicalDisk(blockDiskPath);

    int fd = open(rawDiskPath.c_str(), O_WRONLY);
    if (fd < 0)
    {
        perror("open disk for write-back failed (is USB stick still plugged in?)");
        return;
    }

    std::string line;
    std::vector<uint8_t> slotBuffer(FLOPPY_BYTES);

    while (std::getline(stateIn, line))
    {
        std::istringstream iss(line);
        uint32_t slot = 0;
        std::string devNode, volPath, imgPath;

        if (!(iss >> slot >> devNode))
            continue;
        std::getline(iss >> std::ws, line);
        auto tabPos = line.rfind('\t');
        if (tabPos == std::string::npos)
            continue;
        volPath = line.substr(0, tabPos);
        imgPath = line.substr(tabPos + 1);

        // 1. Strip OS hidden metadata files (.DS_Store, ._*, .Trash-1000, etc.)
        cleanHiddenFiles(volPath);

        // 2. Detach / unmount loop image so filesystem buffers flush to .img
#ifdef __APPLE__
        std::string detachCmd = std::format("hdiutil detach {} >/dev/null 2>&1", devNode);
#elif defined(__linux__)
        std::string detachCmd = std::format("umount {} >/dev/null 2>&1", volPath);
#endif
        if (std::system(detachCmd.c_str()) != 0)
        {
            std::cerr << std::format("Warning: could not unmount {} ({})\n", volPath, devNode);
            continue;
        }

        // 3. Write updated 1.44 MB image back to its exact 1.5 MB stride offset on USB
        std::ifstream in(imgPath, std::ios::binary);
        in.read(reinterpret_cast<char *>(slotBuffer.data()), slotBuffer.size());
        in.close();

        off_t offset = static_cast<off_t>(slot) * SLOT_STRIDE_BYTES;
        if (pwrite(fd, slotBuffer.data(), FLOPPY_BYTES, offset) == static_cast<ssize_t>(FLOPPY_BYTES))
        {
            std::cout << std::format("Saved & detached Slot {:03d} ({})\n", slot, volPath);
        }
        else
        {
            perror("pwrite failed");
        }
    }
#ifdef __linux__
    fs::sync();
    std::error_code ec;
    fs::remove_all(LINUX_MNT_BASE, ec);
#endif
    close(fd);
    stateIn.close();
    fs::remove_all(WORK_DIR);
    std::cout << "All slots synced to USB stick. You can safely unplug it now.\n";
}
void setFat12VolumeLabel(std::vector<uint8_t> &img, uint32_t slotIndex)
{
    std::array<char, 12> label{};
    std::snprintf(label.data(), label.size(), "SLOT_%03u   ", slotIndex % 1000);
    // Boot sector volume label (offset 43)
    std::memcpy(img.data() + 43, label.data(), 11);
    // Unique 4-byte Volume Serial Number (offset 39)
    uint32_t volSerial = 0x11000000U + slotIndex;
    std::memcpy(img.data() + 39, &volSerial, sizeof(volSerial));
    // Root directory volume label entry (Sector 19)
    constexpr size_t ROOT_DIR_OFFSET = 19 * 512;
    std::memcpy(img.data() + ROOT_DIR_OFFSET, label.data(), 11);
    img.at(ROOT_DIR_OFFSET + 11) = 0x08;
}
bool initUsbStick(const DiskInfo &disk, uint32_t firmwareCap = 100)
{
    uint32_t count = std::min(disk.maxPossibleSlots, firmwareCap);
    if (count == 0)
        return false;
    unmountPhysicalDisk(disk.blockPath);
    int fd = open(disk.rawPath.c_str(), O_RDWR);
    if (fd < 0)
    {
        perror("open disk for init failed");
        return false;
    }
    std::vector<uint8_t> templateData(FLOPPY_BYTES);
    if (pread(fd, templateData.data(), FLOPPY_BYTES, 0) != static_cast<ssize_t>(FLOPPY_BYTES))
    {
        perror("failed reading Slot 000 template");
        close(fd);
        return false;
    }
    if (templateData.at(510) != 0x55 || templateData.at(511) != 0xAA)
    {
        std::cerr << "Error: Slot 000 is not formatted yet! Format Slot 000 on your hardware first.\n";
        close(fd);
        return false;
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        setFat12VolumeLabel(templateData, i);
        off_t offset = static_cast<off_t>(i) * SLOT_STRIDE_BYTES;
        if (pwrite(fd, templateData.data(), FLOPPY_BYTES, offset) != static_cast<ssize_t>(FLOPPY_BYTES))
        {
            perror("pwrite failed");
            close(fd);
            return false;
        }
    }
#ifdef __linux__
    fs::sync();
#endif
    close(fd);
    std::cout << std::format("Cloned format to {} slots (SLOT_000..SLOT_{:03d}) on {}\n",
                             count, count - 1, disk.blockPath);
    return true;
}
struct SlotRange
{
    uint32_t start = 0;
    uint32_t end = 999;
};
SlotRange parseRangeArg(const std::string &arg)
{
    SlotRange range;
    auto dashPos = arg.find('-');
    if (dashPos != std::string::npos)
    {
        std::string left = arg.substr(0, dashPos);
        std::string right = arg.substr(dashPos + 1);
        range.start = left.empty() ? 0 : static_cast<uint32_t>(std::stoul(left));
        range.end = right.empty() ? 999 : static_cast<uint32_t>(std::stoul(right));
        if (range.end < range.start)
            std::swap(range.start, range.end);
    }
    else
    {
        uint32_t single = static_cast<uint32_t>(std::stoul(arg));
        range.start = single;
        range.end = single;
    }
    return range;
}

std::string detectExternalUsbDisk()
{
#ifdef __APPLE__
    int rc = 0;
    std::string out = execCommand("diskutil list external physical | grep '/dev/disk' | awk '{print $1}'", rc);
    std::istringstream iss(out);
    std::string firstDisk;
    if (std::getline(iss, firstDisk) && !firstDisk.empty())
    {
        auto pos = firstDisk.find("/dev/");
        if (pos != std::string::npos)
        {
            return firstDisk.substr(pos + 5);
        }
    }
#elif defined(__linux__)
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator("/sys/block", ec))
    {
        std::string devName = entry.path().filename().string();
        if (!devName.starts_with("sd"))
            continue;

        bool isUsb = fs::canonical(entry.path(), ec).string().find("/usb") != std::string::npos;
        std::ifstream remFile(entry.path() / "removable");
        int removable = 0;
        remFile >> removable;

        if (isUsb || removable == 1)
        {
            std::ifstream sizeFile(entry.path() / "size");
            uint64_t sectors = 0;
            if (sizeFile >> sectors && sectors > 0)
            {
                return devName; // e.g., "sdb"
            }
        }
    }
#endif
    return "";
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::cout << "Usage:\n"
                  << "  sudo ./floppy_usb mount [diskX|sdX] [slot | start-end]\n"
                  << "  sudo ./floppy_usb unmount\n"
                  << "  sudo ./floppy_usb init [diskX|sdX] [100|1000]\n";
        return 1;
    }
    std::string cmd = *(argv + 1);
    if (cmd == "unmount")
    {
        unmountAndCleanAllSlots();
        return 0;
    }
    if (cmd != "mount" && cmd != "init")
    {
        std::cerr << "Unknown command: " << cmd << "\n";
        return 1;
    }
    std::string diskId;
    std::string extraArg;
    if (argc >= 3)
    {
        std::string arg2 = *(argv + 2);
        if (arg2.starts_with("disk") || arg2.starts_with("sd") || arg2.starts_with("/dev/"))
        {
            diskId = arg2;
            if (argc >= 4)
                extraArg = *(argv + 3);
        }
        else
        {
            extraArg = arg2;
        }
    }
    if (diskId.empty())
    {
        diskId = detectExternalUsbDisk();
        if (diskId.empty())
        {
            std::cerr << "No external USB stick detected. Plug it in and try again.\n";
            return 1;
        }
        std::cout << "Auto-detected USB stick: " << diskId << "\n";
    }
    DiskInfo disk = getDiskInfo(diskId);
    if (disk.totalBytes == 0)
        return 1;
    try
    {
        if (cmd == "mount")
        {
            SlotRange range{0, 999};
            if (!extraArg.empty())
            {
                range = parseRangeArg(extraArg);
            }
            auto slots = scanExistingSlots(disk, range.start, range.end);
            std::cout << std::format("Found {} formatted slots (range {}..{}).\n",
                                     slots.size(), range.start, range.end);
            mountAllSlots(disk, slots);
        }
        else if (cmd == "init")
        {
            uint32_t cap = extraArg.empty() ? 100 : static_cast<uint32_t>(std::stoul(extraArg));
            initUsbStick(disk, cap);
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Invalid numeric argument: " << e.what() << "\n";
        return 1;
    }
    return 0;
}