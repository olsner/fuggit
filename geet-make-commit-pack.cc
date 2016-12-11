#include <arpa/inet.h>
#include <assert.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <unistd.h>
#include <string>
#include <map>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdarg.h>

using namespace std;

__attribute__((noreturn)) void perror_abort(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    perror("");
    abort();
}

string read_file(const string &name) {
    struct stat buf;
    if (stat(name.c_str(), &buf) < 0) {
        perror_abort("stat: \"%s\"", name.c_str());
    }
    const size_t size = buf.st_size;
    string res(size, ' ');
    FILE *fp = fopen(name.c_str(), "rb");
    size_t pos = 0;
    while (pos < size) {
        ssize_t n = fread(&res[pos], 1, size - pos, fp);
        if (n < 0) {
            perror_abort("fread");
        }
        pos += n;
    }
    assert(res.size() == size);
    fclose(fp);
    return res;
}

enum GitType {
    OBJ_COMMIT = 1,
    OBJ_TREE = 2,
    OBJ_BLOB = 3,
};

string hex(const string& bin) {
    string res;
    for (unsigned char c : bin) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", c);
        res += buf;
    }
    return res;
}

string sha1(const string& data) {
    unsigned char temp[20];
    SHA1((const unsigned char *)data.data(), data.size(), temp);
    return string((const char *)temp, 20);
}

struct Object {
    GitType type;
    const string* data;
};

struct Pack {
    map<string, Object> objects;

    void add(const string& hash, const Object& obj) {
        if (objects.find(hash) == objects.end()) {
            objects[hash] = obj;
        }
    }

    string hash_add(const Object& obj) {
        string hash = sha1(*obj.data);
        add(hash, obj);
        return hash;
    }

    static void write_vint(FILE *fp, size_t x) {
        assert(x);
        while (x) {
            char c = x & 0x7f;
            if (x >>= 7) c |= 0x80;
            fputc(c, fp);
        }
    }

    static void write(FILE *fp, const string& data) {
        size_t size = data.size();
        const char *p = data.data();
        while (size) {
            ssize_t n = fwrite(p, 1, size, fp);
            if (n < 0) perror_abort("fwrite");
            size -= n;
            p += n;
        }
    }

    void print(FILE *fp) {
        char header[] = "PACK\0\0\0\2\0\0\0";
        ((uint32_t*)header)[2] = htonl(objects.size());
        fwrite(header, sizeof(header), 1, fp);
        for (const auto& i: objects) {
            // first byte: low 4 bits is size, high bit is "size cont'd",
            // middle three bits are type
            const string& hash = i.first;
            const Object& obj = i.second;

            assert(obj.type);
            size_t size = obj.data->size();
            write_vint(fp, ((size & -16) << 3) | (obj.type << 4) | (size & 15));
            // FIXME compressed(data) - even if stored, it's in a deflate stream
            write(fp, *obj.data);
        }
    }
};

string branch, commitmessage, parent;
Pack pack;

struct Tree {

    string path_;
    string data_;
    string hash_;
    struct stat stat_;
    map<string, Tree> files_;

    Tree(const string& path): path_(path) {
        ::stat(path.c_str(), &stat_);
    }

    Tree& addpath(const string& path, const size_t pos = 0) {
        size_t end = path.find('/', pos);
        Tree& sub = addfile(path.substr(pos, end), path.substr(0, end));
        if (end != string::npos) {
            sub.addpath(path, end + 1);
        }
    }

    Tree& addfile(const string& name, const string& path) {
        auto it = files_.find(name);
        if (it == files_.end()) {
            return files_.insert(make_pair(name, Tree(path))).first->second;
        } else {
            return it->second;
        }
    }

    const struct stat& stat() {
        return stat_;
    }

    const string& data() {
        if (data_.size() == 0) {
            if (S_ISDIR(stat().st_mode)) {
                for (auto& it: files_) {
                    it.second.hash();
                    // dir: generate dir data
                }
            } else {
                data_ = read_file(path_);
            }
        }
        return data_;
    }

    const string& hash() {
        if (hash_.empty()) {
            hash_ = sha1(data());
            pack.add(hash_, object());
        }
        return hash_;
    }

    GitType gittype() {
        return S_ISDIR(stat().st_mode) ? OBJ_TREE : OBJ_BLOB;
    }

    Object object() {
        return Object { gittype(), &data() };
    }
};

Tree root(".");

string make_commit(const string& tree, const string& commitmessage) {
    const char pfx[] = "commit 254\0tree ";
    return string(pfx, sizeof(pfx) - 1) +
        tree + "\n"
        "parent " + parent + "\n"
        "\n" + commitmessage;
}

// geet-make-commit-pack BRANCH COMMITMSGFILE PARENT
int main(int argc, const char *argv[]) {
    assert(argc >= 4);
    branch = argv[1];
    commitmessage = read_file(argv[2]);
    parent = argv[3];
    assert(parent.size() == 40);

    char *line = nullptr;
    size_t n = 0;
    while (getdelim(&line, &n, '\0', stdin) >= 0)
    {
        root.addpath(line);
    }

    string tree = root.hash();
    string commitobj = make_commit(hex(tree), commitmessage);
    string commithash = pack.hash_add(Object { OBJ_COMMIT, &commitobj });

    printf("%04zx%s %s %s", 6 + 40 + branch.size() + 40,
            parent.c_str(), hex(commithash).c_str(), branch.c_str());
    printf("0000");
    pack.print(stdout);

    FILE *fp = fopen("tmp.pack", "wb");
    pack.print(fp);
    fclose(fp);
}
