#include <arpa/inet.h>
#include <assert.h>
#include <map>
#include <openssl/sha.h>
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <zlib.h>

using namespace std;

namespace {

static const string NUL = string(1, '\0');
static const string SP = " ";
static const string LF = "\n";

__attribute__((noreturn)) void perror_abort(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    perror("");
    abort();
}

string compress(const string &in) {
    string out(compressBound(in.size()), ' ');
    size_t destLen = out.size();
    int status = compress2((Bytef*)&out[0], &destLen, (const Bytef*)&in[0],
            in.size(), 9);
    if (status == Z_OK) {
        out.erase(destLen);
    } else {
        fprintf(stderr, "zlib error: %d (%s)\n", status, zError(status));
        abort();
    }
    return out;
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

string sha1(const string& header, const string& data) {
    SHA_CTX sha;
    SHA1_Init(&sha);
    SHA1_Update(&sha, header.data(), header.size());
    SHA1_Update(&sha, data.data(), data.size());
    unsigned char temp[SHA_DIGEST_LENGTH];
    SHA1_Final(temp, &sha);
    return string((const char *)temp, sizeof(temp));
}

const char *gittype_name(GitType type) {
    switch (type) {
    case OBJ_COMMIT:
        return "commit";
    case OBJ_TREE:
        return "tree";
    case OBJ_BLOB:
        return "blob";
    }
    abort();
}

string git_header(GitType type, size_t length) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%s %zu", gittype_name(type), length);
    // Trailing NUL is included as a separator
    return string(buf, strlen(buf) + 1);
}

struct Object {
    GitType type;
    const string* data;
};

void xfwrite(FILE *fp, const void *p_, size_t size) {
    const char *p = (const char *)p_;
    while (size) {
        ssize_t n = fwrite(p, 1, size, fp);
        if (n < 0) perror_abort("fwrite");
        size -= n;
        p += n;
    }
}
void xfwrite(FILE *fp, const string& data) {
    xfwrite(fp, data.data(), data.size());
}

struct Pack {
    map<string, Object> objects;
    SHA_CTX sha;

    void add(const string& hash, const Object& obj) {
        if (objects.find(hash) == objects.end()) {
            objects[hash] = obj;
        }
    }

    string hash_add(const Object& obj) {
        string hash = sha1(git_header(obj.type, obj.data->size()), *obj.data);
        add(hash, obj);
        return hash;
    }

    void write_vint(FILE *fp, size_t x) {
        assert(x);
        while (x) {
            char c = x & 0x7f;
            if (x >>= 7) c |= 0x80;
            writec(fp, c);
        }
    }

    void writec(FILE *fp, char c) {
        fputc(c, fp);
        SHA1_Update(&sha, &c, 1);
    }
    void write(FILE *fp, const string& data) {
        write(fp, data.data(), data.size());
    }
    void write(FILE *fp, const void *p, size_t size) {
        SHA1_Update(&sha, p, size);
        xfwrite(fp, p, size);
    }

    void print(FILE *fp) {
        SHA1_Init(&sha);
        char header[] = "PACK\0\0\0\2\0\0\0";
        ((uint32_t*)header)[2] = htonl(objects.size());
        write(fp, header, sizeof(header));
        for (const auto& i: objects) {
            // first byte: low 4 bits is size, high bit is "size cont'd",
            // middle three bits are type
            //const string& hash = i.first;
            const Object& obj = i.second;

            assert(obj.type);
            size_t size = obj.data->size();
            long file_pos = ftell(fp);
            write_vint(fp, ((size >> 4) << 7) | (obj.type << 4) | (size & 15));
            long file_pos2 = ftell(fp);
            const string compressed = compress(*obj.data);
            write(fp, compressed);

            if (0 && file_pos > 0) {
                // SHA-1 type size size-in-packfile offset-in-packfile
                fprintf(stderr, "%s %-6s %zu %zu %ld\n",
                        hex(i.first).c_str(),
                        gittype_name(obj.type),
                        size,
                        compressed.size() + (file_pos2 - file_pos),
                        file_pos);
            }
        }
        unsigned char md[SHA_DIGEST_LENGTH];
        SHA1_Final(md, &sha);
        write(fp, md, sizeof(md));
    }
};

string branch, author, commitmessage, parent;
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

    void addpath(const string& path, const size_t pos = 0) {
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
                    string hash = it.second.hash();

                    char buf[12];
                    snprintf(buf, sizeof(buf), "%o ", it.second.git_mode());
                    const string entry = buf + it.first + NUL + hash;
                    data_ += entry;
//                    fprintf(stderr, "%s: %s %s\n", path_.c_str(), entry.c_str(), hex(hash).c_str());
                }
            } else if (S_ISREG(stat().st_mode)) {
                data_ = read_file(path_);
            } else {
                fprintf(stderr, "Unhandled file type (mode %o) for %s\n",
                        stat().st_mode, path_.c_str());
                abort();
            }
        }
        return data_;
    }

    string header() {
        return git_header(gittype(), data().size());
    }

    const string& hash() {
        if (hash_.empty()) {
            hash_ = sha1(header(), data());
//            fprintf(stderr, "%s: %s\n", path_.c_str(), hex(hash_).c_str());
            pack.add(hash_, object());
        }
        return hash_;
    }

    GitType gittype() {
        return S_ISDIR(stat().st_mode) ? OBJ_TREE : OBJ_BLOB;
    }

    int git_mode() {
        int m = stat().st_mode;
        // file types recognized by git
        const int type_mask = S_IFDIR | S_IFREG | S_IFLNK;
        const int exec_mask = 0111;

        if ((m & S_IFMT) != (m & type_mask))
        {
            fprintf(stderr, "Unknown file type (mode %o) for %s\n",
                    m, path_.c_str());
            abort();
        }
        return (m & type_mask) | 0644 | (m & exec_mask ? exec_mask : 0);
    }

    Object object() {
        return Object { gittype(), &data() };
    }
};

Tree root(".");

// output: safe_name <safe_email> git_date
string make_author() {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s %ld +0000", author.c_str(), time(nullptr));
    return buf;
}

string make_commit(const string& tree, const string& commitmessage) {
    const string author = make_author();
    return "tree " + tree +
        "\nparent " + parent +
        "\nauthor " + author +
        "\ncommitter " + author +
        "\n\n" + commitmessage;
}

void pktline(const string& payload) {
    if (payload.size() > 65516) {
        fprintf(stderr, "FATAL: payload too large for pktline format\n");
        abort();
    }

    printf("%04zx", payload.size() + 4);
    xfwrite(stdout, payload);
}

}

// fgt-make-commit-pack BRANCH COMMITMSGFILE PARENT
int main(int argc, const char *argv[]) {
    assert(argc >= 5);
    branch = argv[1];
    author = argv[2];
    commitmessage = read_file(argv[3]);
    parent = argv[4];
    assert(parent.size() == 40);

    char *line = nullptr;
    size_t n = 0;
    while (getdelim(&line, &n, '\0', stdin) >= 0)
    {
        if (string(line) == "./tmp.pack" || string(line) == "./tmp.idx")
            continue;
        const char *path = strchr(line, '/');
        if (path)
            root.addpath(path + 1);
    }

    string tree = root.hash();
    string commitobj = make_commit(hex(tree), commitmessage);
    string commithash = pack.hash_add(Object { OBJ_COMMIT, &commitobj });

    fprintf(stderr, "%s\n", commitobj.c_str());

    const string capabilities = "report-status";
    pktline(parent + SP + hex(commithash) + SP + branch +
            NUL + capabilities + LF);
    printf("0000");
    pack.print(stdout);

    FILE *fp = fopen("tmp.pack", "wb");
    pack.print(fp);
    fclose(fp);
}
