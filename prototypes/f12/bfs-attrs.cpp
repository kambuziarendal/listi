// Wire format: repeated big-endian name length, type, data length, name, data.
#include <ByteOrder.h>
#include <Node.h>
#include <fs_attr.h>
#include <stdint.h>
#include <cstdio>
#include <cstring>
#include <vector>

static bool save(BNode& node)
{
    char name[B_ATTR_NAME_LENGTH];
    status_t status;
    while ((status = node.GetNextAttrName(name)) == B_OK) {
        attr_info info;
        if (node.GetAttrInfo(name, &info) != B_OK || info.size < 0
            || uint64(info.size) > UINT32_MAX)
            return false;
        std::vector<char> data(size_t(info.size) + 1);
        if (node.ReadAttr(name, info.type, 0, data.data(), info.size) != info.size)
            return false;
        uint32 header[] = { B_HOST_TO_BENDIAN_INT32(uint32(strlen(name))),
            B_HOST_TO_BENDIAN_INT32(info.type), B_HOST_TO_BENDIAN_INT32(uint32(info.size)) };
        if (fwrite(header, 1, sizeof(header), stdout) != sizeof(header)
            || fwrite(name, 1, strlen(name), stdout) != strlen(name)
            || fwrite(data.data(), 1, info.size, stdout) != size_t(info.size))
            return false;
    }
    return status == B_ENTRY_NOT_FOUND && fflush(stdout) == 0;
}

static bool load(BNode& node)
{
    uint32 header[3];
    while (true) {
        size_t n = fread(header, 1, sizeof(header), stdin);
        if (n == 0)
            return feof(stdin) != 0;
        if (n != sizeof(header))
            return false;
        uint32 namesize = B_BENDIAN_TO_HOST_INT32(header[0]);
        uint32 type = B_BENDIAN_TO_HOST_INT32(header[1]);
        uint32 size = B_BENDIAN_TO_HOST_INT32(header[2]);
        char name[B_ATTR_NAME_LENGTH] = {};
        if (!namesize || namesize >= sizeof(name) || fread(name, 1, namesize, stdin) != namesize)
            return false;
        std::vector<char> data(size_t(size) + 1);
        if (fread(data.data(), 1, size, stdin) != size
            || node.WriteAttr(name, type, 0, data.data(), size) != size)
            return false;
    }
}

int main(int argc, char** argv)
{
    if (argc != 3)
        return 1;
    BNode node(argv[2]); // BNode opens the link itself, unlike BFile.
    if (node.InitCheck() != B_OK)
        return 1;
    return (strcmp(argv[1], "save") == 0 ? save(node) : load(node)) ? 0 : 1;
}
