// u-config: a small, simple, portable pkg-config clone
// https://github.com/skeeto/u-config
// This is free and unencumbered software released into the public domain.

// Fundamental definitions

#define VERSION "0.30.0"

typedef int Size;
#define Size_MASK ((unsigned)-1)
#define Size_MAX  ((Size)(Size_MASK>>1) - (ALIGN-1))

#define SIZEOF(x) (Size)(sizeof(x))
#define ALIGN SIZEOF(void *)
#define COUNTOF(a) (SIZEOF(a)/SIZEOF(a[0]))

typedef int Bool;
typedef unsigned char Byte;

#if __GNUC__
  #define TRAP __builtin_trap()
  #define NORETURN __attribute__((noreturn))
#elif _MSC_VER
  #define TRAP __debugbreak()
  #define NORETURN __declspec(noreturn)
#else
  #define TRAP *(volatile int *)0 = 0
  #define NORETURN
#endif

#ifdef DEBUG
  #define ASSERT(c) if (!(c)) TRAP
#else
  #define ASSERT(c)
#endif

typedef struct {
    Byte *s;
    Size len;
} Str;

#ifdef __cplusplus
  #define S(s) makestr((Byte *)s, SIZEOF(s)-1)
  static inline Str makestr(Byte *s, Size len)
  {
      Str r = {s, len};
      return r;
  }
#else
  #define S(s) (Str){(Byte *)s, SIZEOF(s)-1}
#endif

typedef struct {
    Str mem;
    Size off;
} Arena;

typedef struct {
    Arena arena;
    Str *args;
    Size nargs;
    Str envpath;      // $PKG_CONFIG_PATH or empty
    Str fixedpath;    // $PKG_CONFIG_LIBDIR or default
    Str top_builddir; // $PKG_CONFIG_TOP_BUILD_DIR or default
    Str sys_incpath;  // $PKG_CONFIG_SYSTEM_INCLUDE_PATH or default
    Str sys_libpath;  // $PKG_CONFIG_SYSTEM_LIBRARY_PATH or default
    Bool define_prefix;
    Byte delim;
} Config;


// Platform API

// Application entry point. Returning from this function indicates the
// application itself completed successfully. However, an os_write error
// may result in a non-zero exit.
static void appmain(Config);

typedef enum {MapFile_OK, MapFile_NOTFOUND, MapFile_READERR} MapFileStatus;

typedef struct {
    Str contents;
    MapFileStatus status;
} MapFileResult;

// Load a file into memory, maybe using the arena. The path must include
// a null terminator since it may be passed directly to the OS interface.
static MapFileResult os_mapfile(Arena *, Str path);

// Write buffer to stdout (1) or stderr (2). The platform must detect
// write errors and arrange for an eventual non-zero exit status.
static void os_write(int fd, Str);

// Immediately exit the program with a non-zero status.
NORETURN static void os_fail(void);


// Application

NORETURN static void oom(void)
{
    os_write(2, S("pkg-config: out of memory\n"));
    os_fail();
}

static Bool digit(Byte c)
{
    return c>='0' && c<='9';
}

static Bool whitespace(Byte c)
{
    switch (c) {
    case '\t': case '\n': case '\b': case '\f': case '\r': case ' ':
        return 1;
    }
    return 0;
}

static Bool pathsep(Byte c)
{
    return c=='/' || c=='\\';
}

static Str fillstr(Str s, Byte b)
{
    for (Size i = 0; i < s.len; i++) {
        s.s[i] = b;
    }
    return s;
}

static void *alloc(Arena *a, Size size)
{
    ASSERT(size >= 0);
    ASSERT(size <= Size_MAX);
    size += -size & (ALIGN - 1);
    Size avail = a->mem.len - a->off;
    if (avail < size) {
        oom();
    }
    Byte *p = a->mem.s + a->off;
    a->off += size;
    return p;
}

static void *allocarray(Arena *a, Size size, Size count)
{
    ASSERT(size > 0);
    ASSERT(count >= 0);
    if (count > Size_MAX/size) {
        oom();
    }
    return alloc(a, size*count);
}

static Str newstr(Arena *a, Size len)
{
    Str r = {(Byte *)alloc(a, len), len};
    return r;
}

static void *zalloc(Arena *a, Size size)
{
    Str r = newstr(a, size);
    return fillstr(r, 0).s;
}

static Str maxstr(Arena *a)
{
    Size len = a->mem.len - a->off;
    return newstr(a, len);
}

// Fill free space with garbage when debugging.
static void shredfree(Arena *a)
{
    (void)a;
    #ifdef DEBUG
    Arena temp = *a;
    fillstr(maxstr(&temp), 0xa5);
    #endif
}

static Str fromptrs(Byte *beg, Byte *end)
{
    ASSERT(beg);
    ASSERT(end);
    ASSERT(end >= beg);
    Str s = {beg, (Size)(end - beg)};
    return s;
}

// Copy src into dst returning the remaining portion of dst.
static Str copy(Str dst, Str src)
{
    ASSERT(dst.len >= src.len);
    for (Size i = 0; i < src.len; i++) {
        dst.s[i] = src.s[i];
    }
    Str r = {dst.s+src.len, dst.len-src.len};
    return r;
}

// Compare strings, returning -1, 0, or +1.
static int orderstr(Str a, Str b)
{
    // NOTE: "null" strings are still valid strings
    Size len = a.len<b.len ? a.len : b.len;
    for (Size i = 0; i < len; i++) {
        int d = a.s[i] - b.s[i];
        if (d) {
            return d < 0 ? -1 : +1;
        }
    }
    if (a.len == b.len) {
        return 0;
    }
    return a.len<b.len ? -1 : +1;
}

static Bool equals(Str a, Str b)
{
    return 0 == orderstr(a, b);
}

static Str cuthead(Str s, Size off)
{
    ASSERT(off >= 0);
    ASSERT(off <= s.len);
    s.s += off;
    s.len -= off;
    return s;
}

static Str takehead(Str s, Size len)
{
    ASSERT(len >= 0);
    ASSERT(len <= s.len);
    s.len = len;
    return s;
}

static Str cuttail(Str s, Size len)
{
    ASSERT(len >= 0);
    ASSERT(len <= s.len);
    Str r = {s.s, s.len-len};
    return r;
}

static Str taketail(Str s, Size len)
{
    return cuthead(s, s.len-len);
}

static Bool startswith(Str s, Str prefix)
{
    return s.len>=prefix.len && equals(takehead(s, prefix.len), prefix);
}

static Size hash(Str s)
{
    unsigned long long h = 257;
    for (Size i = 0; i < s.len; i++) {
        h ^= s.s[i];
        h *= 1111111111111111111;
    }
    h ^= h >> 33;
    return (Size)(h & Size_MASK);
}

typedef struct {
    Str head;
    Str tail;
} StrPair;

static StrPair digits(Str s)
{
    Size i = 0;
    for (; i<s.len && digit(s.s[i]); i++) {}
    StrPair r = {{s.s, i}, {s.s+i, s.len-i}};
    return r;
}

static Bool tokenspace(Byte c)
{
    return whitespace(c) || c==',';
}

static Str skiptokenspace(Str s)
{
    for (; s.len && tokenspace(*s.s); s = cuthead(s, 1)) {}
    return s;
}

static StrPair nexttoken(Str s)
{
    s = skiptokenspace(s);
    Size len = 0;
    for (; len<s.len && !tokenspace(s.s[len]); len++) {}
    StrPair r = {{s.s, len}, cuthead(s, len)};
    return r;
}

typedef struct {
    Str head;
    Str tail;
    Bool ok;
} Cut;

static Cut cut(Str s, Byte delim)
{
    Size len = 0;
    for (; len < s.len; len++) {
        if (s.s[len] == delim) {
            break;
        }
    }
    if (len == s.len) {
        Cut r = {s, cuthead(s, s.len), 0};
        return r;
    }
    Cut r = {{s.s, len}, cuthead(s, len+1), 1};
    return r;
}

// Intrusive treap, embed at the beginning of nodes
typedef struct Treap {
    struct Treap *parent;
    struct Treap *child[2];
    Str key;
} Treap;

// Low-level treap search and insertion. It uses the given size when
// allocating a new node, which must be the size of the node containing
// the intrusive Treap struct. If given no arena, returns null when the
// key is not found.
static void *treapinsert(Arena *a, Treap **t, Str key, Size size)
{
    // Traverse down to a matching node or its leaf location
    Treap *parent = 0;
    Treap **target = t;
    while (*target) {
        parent = *target;
        switch (orderstr(key, parent->key)) {
        case -1: target = parent->child + 0; break;
        case  0: return parent;
        case +1: target = parent->child + 1; break;
        }
    }

    // None found, insert a new leaf
    if (!a) {
        return 0;  // "only browsing, thanks"
    }
    Treap *node = (Treap *)zalloc(a, size);
    node->key = key;
    node->parent = parent;
    *target = node;

    // Randomly rotate the tree according to the hash
    Size keyhash = hash(key);
    while (node->parent && hash(node->parent->key)<keyhash) {
        parent = node->parent;

        // Swap places with parent, also updating grandparent
        node->parent = parent->parent;
        parent->parent = node;
        if (node->parent) {
            int i = node->parent->child[0] == parent;
            node->parent->child[!i] = node;
        } else {
            *t = node;
        }

        // Move the opposing child to the ex-parent
        int i = parent->child[0] == node;
        parent->child[!i] = node->child[i];
        if (node->child[i]) {
            node->child[i]->parent = parent;
        }
        node->child[i] = parent;
    }
    return node;
}

typedef struct {
    Str buf;
    Str avail;
    Arena *a;
    int fd;
} Out;

// Buffered output for os_write().
static Out newoutput(Arena *a, int fd, Size len)
{
    Str buf = newstr(a, len);
    Out out = {buf, buf, 0, fd};
    return out;
}

static Out newnullout(void)
{
    Out out = {0};
    out.fd = -1;
    return out;
}

// Output to a dynamically-grown arena buffer. The arena cannot be used
// again until this buffer is finalized.
static Out newmembuf(Arena *a)
{
    Str max = maxstr(a);
    Out out = {max, max, a, 0};
    return out;
}

// Close the stream and release the arena, returning the result buffer.
static Str finalize(Out *out)
{
    ASSERT(!out->fd);
    Size len = out->buf.len - out->avail.len;
    out->a->off -= out->buf.len;
    return newstr(out->a, len);
}

static void flush(Out *out)
{
    ASSERT(out->fd);
    if (out->buf.len != out->avail.len) {
        Str fill = {out->buf.s, out->buf.len-out->avail.len};
        os_write(out->fd, fill);
        out->avail = out->buf;
    }
}

static void outstr(Out *out, Str s)
{
    if (out->fd == -1) {
        return;  // /dev/null
    }

    if (out->fd == 0) {
        // Output to a memory buffer, not a stream
        if (out->avail.len < s.len) {
            oom();
        }
        out->avail = copy(out->avail, s);
        return;
    }

    // Copy into the stream buffer
    while (s.len) {
        if (out->avail.len >= s.len) {
            out->avail = copy(out->avail, s);
            s.len = 0;
        } else if (out->buf.len==out->avail.len && s.len>=out->buf.len) {
            os_write(out->fd, s);
            s.len = 0;
        } else {
            Size len = out->avail.len;
            Str head = takehead(s, len);
            s = cuthead(s, len);
            out->avail = copy(out->avail, head);
            flush(out);
        }
    }
}

static void outbyte(Out *out, Byte b)
{
    Str s = {&b, 1};
    outstr(out, s);
}

typedef struct Var {
    Treap node;
    Str value;
} Var;

typedef struct {
    Treap *vars;
} Env;

// Return a pointer to the binding so that the caller can choose to fill
// it. The arena is optional. If given, the binding will be created and
// set to a null string. An unallocated, zero-initialized environment is
// a valid empty environment.
static Str *insert(Arena *a, Env *e, Str name)
{
    Var *var = (Var *)treapinsert(a, &e->vars, name, SIZEOF(*var));
    return var ? &var->value : 0;
}

// Try to find the binding in the global environment, then failing that,
// the second environment. Returns a null string if no entry was found.
// An unallocated, zero-initialized environment is valid for lookups.
static Str lookup(Env *global, Env *env, Str name)
{
    Str *s = insert(0, global, name);
    if (s) {
        return *s;
    }
    s = insert(0, env, name);
    if (s) {
        return *s;
    }
    Str r = {0};
    return r;
}

static Str dirname(Str path)
{
    Size len = path.len;
    while (len>0 && !pathsep(path.s[--len])) {}
    return takehead(path, len);
}

static Str basename(Str path)
{
    Size len = path.len;
    for (; len>0 && !pathsep(path.s[len-1]); len--) {}
    return taketail(path, path.len-len);
}

static Str buildpath(Arena *a, Str dir, Str pc)
{
    Str sep = S("/");
    Str suffix = S(".pc\0");
    Size pathlen = dir.len + sep.len + pc.len + suffix.len;
    Str path = newstr(a, pathlen);
    Str p = path;
    p = copy(p, dir);
    p = copy(p, sep);
    p = copy(p, pc);
        copy(p, suffix);
    return path;
}

typedef enum {Pkg_DIRECT=1<<0, Pkg_PUBLIC=1<<1} PkgFlags;

typedef struct Pkg {
    Treap node;
    struct Pkg *list;  // total load order list
    Str path;
    Str realname;
    Str contents;
    Env env;
    int flags;

    #define PKG_NFIELDS 10
    Str name;
    Str description;
    Str url;
    Str version;
    Str requires;
    Str requiresprivate;
    Str conflicts;
    Str libs;
    Str libsprivate;
    Str cflags;
} Pkg;

static Str *fieldbyid(Pkg *p, int id)
{
    ASSERT(id >= 0);
    ASSERT(id < PKG_NFIELDS);
    return &p->name + id;
}

static Str *fieldbyname(Pkg *p, Str name)
{
    static const unsigned char offs[] = {0,4,15,18,25,25,41,50,50,62};
    static const unsigned char lens[] = {4,11,3,7,8,16,9,4,12,6};
    static const Byte fields[] =
        "Name" "Description" "URL" "Version" "Requires.private"
        "Conflicts" "Libs.private" "Cflags";
    for (int i = 0; i < COUNTOF(offs); i++) {
        Str field = {(Byte *)fields+offs[i], lens[i]};
        if (equals(field, name)) {
            return fieldbyid(p, i);
        }
    }
    return 0;
}

typedef struct {
    Treap *pkgs;
    Pkg *head, *tail;
    Size count;
} Pkgs;

// Locate a previously-loaded package, or allocate zero-initialized
// space in the set for a new package.
static Pkg *locate(Arena *a, Pkgs *t, Str realname)
{
    Pkg *p = (Pkg *)treapinsert(a, &t->pkgs, realname, SIZEOF(*p));
    if (!p->realname.s) {
        t->count++;
        p->realname = realname;
        if (!t->head) {
            t->head = t->tail = p;
        } else {
            t->tail->list = p;
            t->tail = p;
        }
    }
    return p;
}

typedef enum {
    Parse_OK,
    Parse_DUPFIELD,
    Parse_DUPVARABLE
} ParseStatus;

typedef struct {
    Pkg pkg;
    Str dupname;
    ParseStatus status;
} ParseResult;

// Return the number of escape bytes at the beginning of the input.
static Size escaped(Str s)
{
    if (startswith(s, S("\\\n"))) {
        return 2;
    }
    if (startswith(s, S("\\\r\n"))) {
        return 3;
    }
    return 0;
}

// Return a copy of the input with the escapes squashed out.
static Str stripescapes(Arena *a, Str s)
{
    Size len = 0;
    Str c = newstr(a, s.len);
    for (Size i = 0; i < s.len; i++) {
        Byte b = s.s[i];
        if (b == '\\') {
            Size r = escaped(cuthead(s, i));
            if (r) {
                i += r - 1;
            } else if (i<s.len-1 && s.s[i+1]=='#') {
                // do not store escape
            } else {
                c.s[len++] = b;
            }
        } else {
            c.s[len++] = b;
        }
    }
    return takehead(c, len);
}

static ParseResult parsepackage(Arena *a, Str src)
{
    Byte *p = src.s;
    Byte *e = src.s + src.len;
    ParseResult result = {0};
    result.status = Parse_OK;
    result.pkg.contents = src;

    while (p < e) {
        for (; p<e && whitespace(*p); p++) {}
        if (p<e && *p=='#') {
            while (p<e && *p++!='\n') {}
            continue;
        }

        Byte *beg = p;
        Byte *end = p;
        Byte c = 0;
        while (p < e) {
            c = *p++;
            if (c=='\n' || c=='=' || c==':') {
                break;
            }
            end = whitespace(c) ? end : p;
        }

        Str name = fromptrs(beg, end);
        Str *field = 0;
        switch (c) {
        default:
            continue;

        case '=':
            field = insert(a, &result.pkg.env, name);
            if (field->s) {
                ParseResult dup = {0};
                dup.dupname = name;
                dup.status = Parse_DUPVARABLE;
                return dup;
            }
            break;

        case ':':
            field = fieldbyname(&result.pkg, name);
            if (field && field->s) {
                ParseResult dup = {0};
                dup.dupname = name;
                dup.status = Parse_DUPFIELD;
                return dup;
            }
            break;
        }

        // Skip leading space; newlines may be escaped with a backslash
        while (p < e) {
            if (*p == '\\') {
                Size r = escaped(fromptrs(p, e));
                if (r) {
                    p += r;
                } else {
                    break;
                }
            } else if (*p=='\n' || !whitespace(*p)) {
                break;
            } else {
                p++;
            }
        }

        Bool cleanup = 0;
        end = beg = p;
        for (; p<e && *p!='\n'; p++) {
            if (*p == '#') {
                while (p<e && *p++!='\n') {}
                break;
            } else if (*p == '\\') {
                if (p<e-1 && p[1]=='#') {
                    // Escaped #, include in token and skip over
                    p++;
                    end = p + 1;
                    cleanup = 1;
                }
                Size r = escaped(fromptrs(p, e));
                if (r) {
                    // Escaped newline, skip over
                    p += r - 1;
                    cleanup = 1;
                }
            } else {
                end = whitespace(*p) ? end : p+1;
            }
        }

        if (field) {
            *field = fromptrs(beg, end);
            if (cleanup) {
                // Field contains excess characters. Contents must be
                // modified, so save a copy of it instead.
                *field = stripescapes(a, *field);
            }
        }
    }

    return result;
}

static void missing(Out *err, Str option)
{
    outstr(err, S("pkg-config: "));
    outstr(err, S("argument missing for -"));
    outstr(err, option);
    outbyte(err, '\n');
    flush(err);
    os_fail();
}

typedef struct {
    Size nargs;
    Str *args;
    Size index;
} OptionParser;

static OptionParser newoptionparser(Str *args, Size nargs)
{
    OptionParser r = {nargs, args, 0};
    return r;
}

typedef struct {
    Str arg;
    Str value;
    Bool isoption;
    Bool ok;
} OptionResult;

static OptionResult nextoption(OptionParser *p)
{
    if (p->index == p->nargs) {
        OptionResult r = {0};
        return r;
    }

    Str arg = p->args[p->index++];
    if (arg.len<2 || arg.s[0]!='-') {
        OptionResult r = {0};
        r.arg = arg;
        r.ok = 1;
        return r;
    }

    OptionResult r = {0};
    r.isoption = 1;
    r.ok = 1;
    arg = cuthead(arg, 1);
    Cut c = cut(arg, '=');
    if (c.ok) {
        r.arg = c.head;
        r.value = c.tail;
    } else {
        r.arg = arg;
    }
    return r;
}

static Str getargopt(Out *err, OptionParser *p, Str option)
{
    if (p->index == p->nargs) {
        missing(err, option);
    }
    return p->args[p->index++];
}

static void usage(Out *out)
{
    static const char usage[] =
    "u-config " VERSION " https://github.com/skeeto/u-config "
    "(released into the public domain)\n"
    "usage: pkg-config [OPTIONS...] [PACKAGES...]\n"
    "  --cflags, --cflags-only-I, --cflags-only-other\n"
    "  --define-prefix, --dont-define-prefix\n"
    "  --define-variable=NAME=VALUE, --variable=NAME\n"
    "  --exists\n"
    "  --keep-system-cflags, --keep-system-libs\n"
    "  --libs, --libs-only-L, --libs-only-l, --libs-only-other\n"
    "  --maximum-traverse-depth=N\n"
    "  --modversion\n"
    "  --msvc-syntax\n"
    "  --newlines\n"
    "  --static\n"
    "  --validate\n"
    "  --with-path=PATH\n"
    "  -h, --help, --version\n";
    outstr(out, S(usage));
}

typedef struct SearchNode {
    struct SearchNode *next;
    Str dir;
} SearchNode;

typedef struct {
    SearchNode *head;
    SearchNode *tail;
    Byte delim;
} Search;

static Search newsearch(Byte delim)
{
    Search r = {0};
    r.delim = delim;
    return r;
}

static void appenddir(Arena *a, Search *dirs, Str dir)
{
    SearchNode *node = (SearchNode *)alloc(a, SIZEOF(*node));
    node->dir = dir;
    node->next = 0;
    if (dirs->tail) {
        ASSERT(dirs->head);
        dirs->tail->next = node;
    } else {
        ASSERT(!dirs->tail);
        dirs->head = node;
    }
    dirs->tail = node;
}

static void appendpath(Arena *a, Search *dirs, Str path)
{
    while (path.len) {
        Cut c = cut(path, dirs->delim);
        Str dir = c.head;
        if (dir.len) {
            appenddir(a, dirs, dir);
        }
        path = c.tail;
    }
}

static void prependpath(Arena *a, Search *dirs, Str path)
{
    if (!dirs->head) {
        // Empty, so appending is the same a prepending
        appendpath(a, dirs, path);
    } else {
        // Append to an empty Search, then transplant it
        Search temp = newsearch(dirs->delim);
        appendpath(a, &temp, path);
        temp.tail->next = dirs->head;
        dirs->head = temp.head;
    }
}

static Bool realnameispath(Str realname)
{
    return realname.len>3 && equals(taketail(realname, 3), S(".pc"));
}

static Str pathtorealname(Str path)
{
    if (!realnameispath(path)) {
        return path;
    }

    Size baselen = 0;
    for (Size i = 0; i < path.len; i++) {
        if (pathsep(path.s[i])) {
            baselen = i + 1;
        }
    }
    Str name = cuthead(path, baselen);
    return cuttail(name, 3);
}

static Str readpackage(Arena *a, Out *err, Str path, Str realname)
{
    if (equals(realname, S("pkg-config"))) {
        return S(
            "Name: u-config\n"
            "Version: " VERSION "\n"
            "Description:\n"
        );
    }

    Str null = {0};
    MapFileResult m = os_mapfile(a, path);
    switch (m.status) {
    case MapFile_NOTFOUND:
        return null;

    case MapFile_READERR:
        outstr(err, S("pkg-config: "));
        outstr(err, S("could not read package '"));
        outstr(err, realname);
        outstr(err, S("' from '"));
        outstr(err, path);
        outstr(err, S("'\n"));
        flush(err);
        os_fail();

    case MapFile_OK:
        return m.contents;
    }
    ASSERT(0);
    return null;
}

static void expand(Out *out, Out *err, Env *global, Pkg *p, Str str)
{
    int top = 0;
    Str stack[128];

    stack[top] = str;
    while (top >= 0) {
        Str s = stack[top--];
        for (Size i = 0; i < s.len-1; i++) {
            if (s.s[i]=='$' && s.s[i+1]=='{') {
                if (top >= COUNTOF(stack)-2) {
                    outstr(err, S("pkg-config: "));
                    outstr(err, S("exceeded max recursion depth in '"));
                    outstr(err, p->path);
                    outstr(err, S("'\n"));
                    flush(err);
                    os_fail();
                }

                Str head = {s.s, i};
                outstr(out, head);

                Size beg = i + 2;
                Size end = beg;
                for (; end<s.len && s.s[end]!='}'; end++) {}
                Str name = {s.s+beg, end-beg};
                end += end < s.len;

                // If the tail is empty, this stack push could be elided
                // as a kind of tail call optimization. However, there
                // would need to be another mechanism in place to detect
                // infinite recursion.
                Str tail = {s.s+end, s.len-end};
                stack[++top] = tail;

                Str value = lookup(global, &p->env, name);
                if (!value.s) {
                    outstr(err, S("pkg-config: "));
                    outstr(err, S("undefined variable '"));
                    outstr(err, name);
                    outstr(err, S("' in '"));
                    outstr(err, p->path);
                    outstr(err, S("'\n"));
                    flush(err);
                    os_fail();
                }
                stack[++top] = value;
                s.len = 0;
                break;

            } else if (s.s[i]=='$' && s.s[i+1]=='$') {
                Str head = {s.s, i+1};
                outstr(out, head);
                Str tail = {s.s+i+2, s.len-i-2};
                stack[++top] = tail;
                s.len = 0;
                break;
            }
        }
        outstr(out, s);
    }
}

// Merge and expand data from "update" into "base".
static void expandmerge(Arena *a, Out *err, Env *g, Pkg *base, Pkg *update)
{
    base->path = update->path;
    base->contents = update->contents;
    base->env = update->env;
    base->flags = update->flags;
    for (int i = 0; i < PKG_NFIELDS; i++) {
        Out mem = newmembuf(a);
        Str src = *fieldbyid(update, i);
        expand(&mem, err, g, update, src);
        *fieldbyid(base, i) = finalize(&mem);
    }
}

static Pkg findpackage(Arena *a, Search *dirs, Out *err, Str realname)
{
    Str path = {0, 0};
    Str contents = {0, 0};

    if (realnameispath(realname)) {
        path = newstr(a, realname.len+1);
        copy(path, realname).s[0] = 0;
        contents = readpackage(a, err, path, realname);
        path = cuttail(path, 1);  // remove null terminator
        if (contents.s) {
            realname = pathtorealname(path);
        }
    }

    for (SearchNode *n = dirs->head; n && !contents.s; n = n->next) {
        path = buildpath(a, n->dir, realname);
        contents = readpackage(a, err, path, realname);
        path = cuttail(path, 1);  // remove null terminator
    }

    if (!contents.s) {
        outstr(err, S("pkg-config: "));
        outstr(err, S("could not find package '"));
        outstr(err, realname);
        outstr(err, S("'\n"));
        flush(err);
        os_fail();
    }

    ParseResult r = parsepackage(a, contents);
    switch (r.status) {
    case Parse_DUPVARABLE:
        outstr(err, S("pkg-config: "));
        outstr(err, S("duplicate variable '"));
        outstr(err, r.dupname);
        outstr(err, S("' in '"));
        outstr(err, path);
        outstr(err, S("'\n"));
        flush(err);
        os_fail();

    case Parse_DUPFIELD:
        outstr(err, S("pkg-config: "));
        outstr(err, S("duplicate field '"));
        outstr(err, r.dupname);
        outstr(err, S("' in '"));
        outstr(err, path);
        outstr(err, S("'\n"));
        flush(err);
        os_fail();

    case Parse_OK:
        break;
    }
    r.pkg.path = path;
    r.pkg.realname = realname;
    *insert(a, &r.pkg.env, S("pcfiledir")) = dirname(path);

    Str missing = {0, 0};
    if (!r.pkg.name.s) {
        missing = S("Name");
    } else if (!r.pkg.version.s) {
        missing = S("Version");
    } else if (!r.pkg.description.s) {
        missing = S("Description");
    }
    if (missing.s) {
        outstr(err, S("pkg-config: "));
        outstr(err, S("missing field '"));
        outstr(err, missing);
        outstr(err, S("' in '"));
        outstr(err, r.pkg.path);
        outstr(err, S("'\n"));
        flush(err);
        #ifndef DEBUG
        // Do not enforce during fuzzing
        os_fail();
        #endif
    }

    return r.pkg;
}

typedef struct {
    Str arg;
    Str tail;
    Bool ok;
} DequoteResult;

static Bool shellmeta(Byte c)
{
    // NOTE: matches pkg-config's listing, which excludes "$()"
    Str meta = S("\"!#%&'*<>?[\\]`{|}");
    for (Size i = 0; i < meta.len; i++) {
        if (meta.s[i] == c) {
            return 1;
        }
    }
    return 0;
}

// Process the next token. Return it and the unprocessed remainder.
static DequoteResult dequote(Arena *a, Str s)
{
    Size i;
    Byte quote = 0;
    Bool escaped = 0;
    Arena save = *a;
    Out mem = newmembuf(a);

    for (; s.len && whitespace(*s.s); s = cuthead(s, 1)) {}

    for (i = 0; i < s.len; i++) {
        Byte c = s.s[i];
        if (whitespace(c)) {
            c = ' ';
        }

        if (quote == '\'') {
            if (c == '\'') {
                quote = 0;
            } else if (c==' ' || shellmeta(c)) {
                outbyte(&mem, '\\');
                outbyte(&mem, c);
            } else {
                outbyte(&mem, c);
            }

        } else if (quote == '"') {
            if (escaped) {
                escaped = 0;
                if (c!='\\' && c!='"') {
                    outbyte(&mem, '\\');
                    if (c==' ' || shellmeta(c)) {
                        outbyte(&mem, '\\');
                    }
                }
                outbyte(&mem, c);
            } else if (c == '\"') {
                quote = 0;
            } else if (c==' ' || shellmeta(c)) {
                outbyte(&mem, '\\');
                outbyte(&mem, c);
            } else {
                escaped = c == '\\';
                outbyte(&mem, c);
            }

        } else if (c=='\'' || c=='"') {
            quote = c;

        } else if (shellmeta(c)) {
            outbyte(&mem, '\\');
            outbyte(&mem, c);

        } else if (c==' ') {
            break;

        } else {
            outbyte(&mem, c);
        }
    }

    if (quote) {
        *a = save;
        shredfree(a);
        DequoteResult r = {0};
        return r;
    }

    DequoteResult r = {finalize(&mem), cuthead(s, i), 1};
    return r;
}

// Compare version strings, returning [-1, 0, +1]. Follows the RPM
// version comparison specification like the original pkg-config.
static int compareversions(Str va, Str vb)
{
    Size i = 0;
    while (i<va.len && i<vb.len) {
        Byte a = va.s[i];
        Byte b = vb.s[i];
        if (!digit(a) || !digit(b)) {
            if (a < b) {
                return -1;
            } else if (a > b) {
                return +1;
            }
            i++;
        } else {
            StrPair pa = digits(cuthead(va, i));
            StrPair pb = digits(cuthead(vb, i));
            if (pa.head.len < pb.head.len) {
                return -1;
            } else if (pa.head.len > pb.head.len) {
                return +1;
            }
            for (i = 0; i < pa.head.len; i++) {
                a = pa.head.s[i];
                b = pb.head.s[i];
                if (a < b) {
                    return -1;
                } else if (a > b) {
                    return +1;
                }
            }
            va = pa.tail;
            vb = pb.tail;
            i = 0;
        }
    }
    if (va.len < vb.len) {
        return -1;
    } else if (va.len > vb.len) {
        return +1;
    }
    return 0;
}

typedef enum {
    VersionOp_ERR=0,
    VersionOp_LT,
    VersionOp_LTE,
    VersionOp_EQ,
    VersionOp_GTE,
    VersionOp_GT
} VersionOp;

static VersionOp parseop(Str s)
{
    if (equals(S("<"), s)) {
        return VersionOp_LT;
    } else if (equals(S("<="), s)) {
        return VersionOp_LTE;
    } else if (equals(S("="), s)) {
        return VersionOp_EQ;
    } else if (equals(S(">="), s)) {
        return VersionOp_GTE;
    } else if (equals(S(">"), s)) {
        return VersionOp_GT;
    }
    return VersionOp_ERR;
}

static Str opname(VersionOp op)
{
    switch (op) {
    case VersionOp_ERR: break;
    case VersionOp_LT:  return S("<");
    case VersionOp_LTE: return S("<=");
    case VersionOp_EQ:  return S("=");
    case VersionOp_GTE: return S(">=");
    case VersionOp_GT:  return S(">");
    }
    ASSERT(0);
    Str null = {0};
    return null;
}

static Bool validcompare(VersionOp op, int result)
{
    switch (op) {
    case VersionOp_ERR: break;
    case VersionOp_LT:  return result <  0;
    case VersionOp_LTE: return result <= 0;
    case VersionOp_EQ:  return result == 0;
    case VersionOp_GTE: return result >= 0;
    case VersionOp_GT:  return result >  0;
    }
    ASSERT(0);
    return 0;
}

typedef struct {
    Out *err;
    Search search;
    Env *global;
    Pkgs *pkgs;
    Pkg *last;
    int maxdepth;
    VersionOp op;
    Bool define_prefix;
    Bool recursive;
} Processor;

static Processor newprocessor(Config *c, Out *err, Env *g, Pkgs *pkgs)
{
    Arena *a = &c->arena;
    Search search = newsearch(c->delim);
    appendpath(a, &search, c->envpath);
    appendpath(a, &search, c->fixedpath);
    int maxdepth = (unsigned)-1 >> 1;
    Processor proc = {err, search, g, pkgs, 0, maxdepth, VersionOp_ERR, 1, 1};
    return proc;
}

static void procfail(Out *err, VersionOp op, Pkg *p)
{
    outstr(err, S("pkg-config: "));
    outstr(err, S("expected version following operator "));
    outstr(err, opname(op));
    if (p) {
        outstr(err, S(" in package '"));
        outstr(err, p->realname);
        outstr(err, S("'"));
    }
    outstr(err, S("\n"));
    flush(err);
    os_fail();
}

// Wrap the string in quotes if it contains whitespace.
static Str maybequote(Arena *a, Str s)
{
    for (Size i = 0; i < s.len; i++) {
        if (whitespace(s.s[i])) {
            Str r = newstr(a, s.len + 2);
            Str t = copy(r, S("\""));
                t = copy(t, s);
                    copy(t, S("\""));
            return r;
        }
    }
    return s;
}

static void setprefix(Arena *a, Pkg *p)
{
    Str parent = dirname(p->path);
    if (equals(S("pkgconfig"), basename(parent))) {
        Str prefix = dirname(dirname(parent));
        prefix = maybequote(a, prefix);
        *insert(a, &p->env, S("prefix")) = prefix;
    }
}

typedef struct {
    Str arg;
    Pkg *last;
    short depth;
    short flags;
    VersionOp op;
} ProcState;

static void failmaxrecurse(Out *err, Str tok)
{
    outstr(err, S("pkg-config: "));
    outstr(err, S("exceeded max recursion depth on '"));
    outstr(err, tok);
    outstr(err, S("'\n"));
    flush(err);
    os_fail();
}

static void process(Arena *a, Processor *proc, Str arg)
{
    Out *err = proc->err;
    Pkgs *pkgs = proc->pkgs;
    Env *global = proc->global;
    Search *search = &proc->search;

    // NOTE: At >=128, GCC generates a __chkstk_ms on x86-64 because the
    // stack frame exceeds 4kB. A -mno-stack-arg-probe solves this, but
    // limiting the recursion depth to 64, which is still plenty, avoids
    // the issue entirely.
    ProcState stack[64];
    int top = 0;
    stack[0].arg = arg;
    stack[0].last = proc->last;
    stack[0].depth = 0;
    stack[0].flags = Pkg_DIRECT | Pkg_PUBLIC;
    stack[0].op = VersionOp_ERR;

    while (top >= 0) {
        ProcState *s = stack + top;
        StrPair pair = nexttoken(s->arg);
        Str tok = pair.head;
        if (!tok.len) {
            if (s->op) {
                procfail(err, s->op, s->last);
            }
            top--;
            continue;
        }
        stack[top].arg = pair.tail;

        if (s->op) {
            int cmp = compareversions(s->last->version, tok);
            if (!validcompare(s->op, cmp)) {
                outstr(err, S("pkg-config: "));
                outstr(err, S("requested '"));
                outstr(err, s->last->realname);
                outstr(err, S("' "));
                outstr(err, opname(s->op));
                outstr(err, S(" '"));
                outstr(err, tok);
                outstr(err, S("' but got '"));
                outstr(err, s->last->version);
                outstr(err, S("'\n"));
                flush(err);
                os_fail();
            }
            s->last = 0;
            s->op = VersionOp_ERR;
            continue;
        }

        s->op = parseop(tok);
        if (s->op) {
            if (!s->last) {
                outstr(err, S("pkg-config: "));
                outstr(err, S("unexpected operator '"));
                outstr(err, tok);
                outstr(err, S("'\n"));
                flush(err);
                os_fail();
            }
            continue;
        }

        short depth = s->depth + 1;
        short flags = s->flags;
        Pkg *pkg = s->last = locate(a, pkgs, pathtorealname(tok));
        if (pkg->contents.s) {
            if (flags&Pkg_PUBLIC && !(pkg->flags & Pkg_PUBLIC)) {
                // We're on a public branch, but this package was
                // previously loaded as private. Recursively traverse
                // its public requires and mark all as public.
                pkg->flags |= Pkg_PUBLIC;
                if (proc->recursive && depth<proc->maxdepth) {
                    if (top >= COUNTOF(stack)-1) {
                        failmaxrecurse(err, tok);
                    }
                    top++;
                    stack[top].arg = pkg->requires;
                    stack[top].last = 0;
                    stack[top].depth = depth;
                    stack[top].flags = (flags & ~Pkg_DIRECT) | flags;
                    stack[top].op = VersionOp_ERR;
                }
            }
        } else {
            // Package hasn't been loaded yet, so find and load it.
            Pkg newpkg = findpackage(a, search, err, tok);
            if (proc->define_prefix) {
                setprefix(a, &newpkg);
            }
            expandmerge(a, err, global, pkg, &newpkg);
            if (proc->recursive && depth<proc->maxdepth) {
                if (top >= COUNTOF(stack)-2) {
                    failmaxrecurse(err, tok);
                }
                top++;
                stack[top].arg = pkg->requiresprivate;
                stack[top].last = 0;
                stack[top].depth = depth;
                stack[top].flags = 0;
                stack[top].op = VersionOp_ERR;
                top++;
                stack[top].arg = pkg->requires;
                stack[top].last = 0;
                stack[top].depth = depth;
                stack[top].flags = (flags & ~Pkg_DIRECT) | flags;
                stack[top].op = VersionOp_ERR;
            }
        }
        pkg->flags |= flags;
    }

    proc->last = stack[0].last;
}

static void endprocessor(Processor *proc, Out *err)
{
    if (proc->op) {
        procfail(err, proc->op, 0);
    }
}

typedef enum {
    Filter_ANY,
    Filter_I,
    Filter_L,
    Filter_l,
    Filter_OTHERC,
    Filter_OTHERL
} Filter;

static Bool filterok(Filter f, Str arg)
{
    switch (f) {
    case Filter_ANY:
        return 1;
    case Filter_I:
        return startswith(arg, S("-I"));
    case Filter_L:
        return startswith(arg, S("-L"));
    case Filter_l:
        return startswith(arg, S("-l"));
    case Filter_OTHERC:
        return !startswith(arg, S("-I"));
    case Filter_OTHERL:
        return !startswith(arg, S("-L")) && !startswith(arg, S("-l"));
    }
    ASSERT(0);
    return 0;
}

static void msvcize(Out *out, Str arg)
{
    if (startswith(arg, S("-L"))) {
        outstr(out, S("/libpath:"));
        outstr(out, cuthead(arg, 2));
    } else if (startswith(arg, S("-I"))) {
        outstr(out, S("/I"));
        outstr(out, cuthead(arg, 2));
    } else if (startswith(arg, S("-l"))) {
        outstr(out, cuthead(arg, 2));
        outstr(out, S(".lib"));
    } else if (startswith(arg, S("-D"))) {
        outstr(out, S("/D"));
        outstr(out, cuthead(arg, 2));
    } else if (equals(arg, S("-mwindows"))) {
        outstr(out, S("/subsystem:windows"));
    } else if (equals(arg, S("-mconsole"))) {
        outstr(out, S("/subsystem:console"));
    } else {
        outstr(out, arg);
    }
}

typedef struct {
    Treap node;
    Bool present;
} StrSetEntry;

typedef struct {
    Treap *set;
} StrSet;

// Try to insert the string into the set, returning true on success.
static Bool insertstr(Arena *a, StrSet *set, Str s)
{
    StrSetEntry *e = (StrSetEntry *)treapinsert(a, &set->set, s, SIZEOF(*e));
    if (!e->present) {
        e->present = 1;
        return 1;
    }
    return 0;
}

typedef struct {
    Arena *arena;
    Out *out;
    Out *err;
    Size count;
    StrSet seen;
    Filter filter;
    Bool msvc;
    Byte delim;
} OutConfig;

static OutConfig newoutconf(Arena *a, Out *out, Out *err)
{
    OutConfig r = {a, out, err, 0, {0}, Filter_ANY, 0, ' '};
    return r;
}

static void insertsyspath(OutConfig *conf, Str path, Byte delim, Byte flag)
{
    Byte flagbuf[] = {'-', flag};
    Str prefix = {flagbuf, SIZEOF(flagbuf)};

    while (path.len) {
        // Allocations are tentative and may be discarded
        Arena save = *conf->arena;

        Cut c = cut(path, delim);
        Str dir = c.head;
        path = c.tail;
        if (!dir.len) {
            continue;
        }

        // Prepend option flag
        Str syspath = newstr(&save, prefix.len+dir.len);
        copy(copy(syspath, prefix), dir);

        // Process as an argument, as though being printed
        syspath = maybequote(&save, syspath);
        DequoteResult dr = dequote(&save, syspath);
        syspath = dr.arg;

        // NOTE(NRK): Technically, the path doesn't need to follow the flag
        // immediately e.g `-I /usr/include` (notice the space between -I and
        // the include dir!).
        //
        // But I have not seen a single pc file that does this and so we're not
        // handling this edge-case. As a proof that this should be fine in
        // practice, `pkgconf` which is used by many distros, also doesn't
        // handle it.
        if (dr.ok && !dr.tail.len && insertstr(&save, &conf->seen, syspath)) {
            *conf->arena = save;
        }
    }
}

// Process the field while writing it to the output.
static void fieldout(OutConfig *conf, Pkg *p, Str field)
{
    Arena *a = conf->arena;
    Filter f = conf->filter;
    while (field.len) {
        Arena tentative = *a;
        DequoteResult r = dequote(&tentative, field);
        if (!r.ok) {
            outstr(conf->err, S("pkg-config: "));
            outstr(conf->err, S("unmatched quote in '"));
            outstr(conf->err, p->realname);
            outstr(conf->err, S("'\n"));
            flush(conf->err);
            os_fail();
        }
        if (filterok(f, r.arg) && insertstr(&tentative, &conf->seen, r.arg)) {
            *a = tentative;  // keep the allocations
            if (conf->count++) {
                outbyte(conf->out, conf->delim);
            }
            if (conf->msvc) {
                msvcize(conf->out, r.arg);
            } else {
                outstr(conf->out, r.arg);
            }
        }
        field = r.tail;
    }
}

static int parseuint(Str s, int hi)
{
    int v = 0;
    for (Size i = 0; i < s.len; i++) {
        Byte c = s.s[i];
        if (digit(c)) {
            v = v*10 + c - '0';
            if (v >= hi) {
                return hi;
            }
        }
    }
    return v;
}

static void appmain(Config conf)
{
    Arena *a = &conf.arena;
    shredfree(a);

    Env global = {0};
    Filter filterc = Filter_ANY;
    Filter filterl = Filter_ANY;
    Pkgs pkgs = {0};
    Out out = newoutput(a, 1, 1<<12);
    Out err = newoutput(a, 2, 1<<7);
    Processor proc = newprocessor(&conf, &err, &global, &pkgs);
    OutConfig outconf = newoutconf(a, &out, &err);

    Bool libs = 0;
    Bool cflags = 0;
    Bool silent = 0;
    Bool static_ = 0;
    Bool modversion = 0;
    Bool print_sysinc = 0;
    Bool print_syslib = 0;
    Str variable = {0, 0};

    proc.define_prefix = conf.define_prefix;
    if (!conf.top_builddir.s) {
        conf.top_builddir = S("$(top_builddir)");
    }

    *insert(a, &global, S("pc_path")) = conf.fixedpath;
    *insert(a, &global, S("pc_system_includedirs")) = conf.sys_incpath;
    *insert(a, &global, S("pc_system_libdirs")) = conf.sys_libpath;
    *insert(a, &global, S("pc_sysrootdir")) = S("/");
    *insert(a, &global, S("pc_top_builddir")) = conf.top_builddir;

    Str *args = (Str *)allocarray(a, SIZEOF(Str), conf.nargs);
    Size nargs = 0;

    for (OptionParser opts = newoptionparser(conf.args, conf.nargs);;) {
        OptionResult r = nextoption(&opts);
        if (!r.ok) {
            break;
        }

        if (!r.isoption) {
            args[nargs++] = r.arg;

        } else if (equals(r.arg, S("h")) || equals(r.arg, S("-help"))) {
            usage(&out);
            flush(&out);
            return;

        } else if (equals(r.arg, S("-version"))) {
            outstr(&out, S(VERSION));
            outbyte(&out, '\n');
            flush(&out);
            return;

        } else if (equals(r.arg, S("-modversion"))) {
            modversion = 1;

        } else if (equals(r.arg, S("-define-prefix"))) {
            proc.define_prefix = 1;

        } else if (equals(r.arg, S("-dont-define-prefix"))) {
            proc.define_prefix = 0;

        } else if (equals(r.arg, S("-cflags"))) {
            cflags = 1;
            filterc = Filter_ANY;

        } else if (equals(r.arg, S("-libs"))) {
            libs = 1;
            filterl = Filter_ANY;

        } else if (equals(r.arg, S("-variable"))) {
            if (!r.value.s) {
                r.value = getargopt(&err, &opts, r.arg);
            }
            variable = r.value;

        } else if (equals(r.arg, S("-static"))) {
            static_ = 1;

        } else if (equals(r.arg, S("-libs-only-L"))) {
            libs = 1;
            filterl = Filter_L;

        } else if (equals(r.arg, S("-libs-only-l"))) {
            libs = 1;
            filterl = Filter_l;

        } else if (equals(r.arg, S("-libs-only-other"))) {
            libs = 1;
            filterl = Filter_OTHERL;

        } else if (equals(r.arg, S("-cflags-only-I"))) {
            cflags = 1;
            filterc = Filter_I;

        } else if (equals(r.arg, S("-cflags-only-other"))) {
            cflags = 1;
            filterc = Filter_OTHERC;

        } else if (equals(r.arg, S("-with-path"))) {
            if (!r.value.s) {
                r.value = getargopt(&err, &opts, r.arg);
            }
            prependpath(a, &proc.search, r.value);

        } else if (equals(r.arg, S("-maximum-traverse-depth"))) {
            if (!r.value.s) {
                r.value = getargopt(&err, &opts, r.arg);
            }
            proc.maxdepth = parseuint(r.value, 1000);

        } else if (equals(r.arg, S("-msvc-syntax"))) {
            outconf.msvc = 1;

        } else if (equals(r.arg, S("-define-variable"))) {
            if (!r.value.s) {
                r.value = getargopt(&err, &opts, r.arg);
            }
            Cut c = cut(r.value, '=');
            if (!c.ok) {
                outstr(&err, S("pkg-config: "));
                outstr(&err, S("value missing in --define-variable for '"));
                outstr(&err, r.value);
                outstr(&err, S("'\n"));
                flush(&err);
                os_fail();
            }
            *insert(a, &global, c.head) = c.tail;

        } else if (equals(r.arg, S("-newlines"))) {
            outconf.delim = '\n';

        } else if (equals(r.arg, S("-exists"))) {
            // The check already happens, just disable the messages
            silent = 1;

        } else if (equals(r.arg, S("-atleast-pkgconfig-version"))) {
            if (!r.value.s) {
                r.value = getargopt(&err, &opts, r.arg);
            }
            return;  // always succeeds

        } else if (equals(r.arg, S("-print-errors"))) {
            // Ignore

        } else if (equals(r.arg, S("-short-errors"))) {
            // Ignore

        } else if (equals(r.arg, S("-keep-system-cflags"))) {
            print_sysinc = 1;

        } else if (equals(r.arg, S("-keep-system-libs"))) {
            print_syslib = 1;

        } else if (equals(r.arg, S("-validate"))) {
            silent = 1;
            proc.recursive = 0;

        } else {
            outstr(&err, S("pkg-config: "));
            outstr(&err, S("unknown option -"));
            outstr(&err, r.arg);
            outstr(&err, S("\n"));
            flush(&err);
            os_fail();
        }
    }

    if (silent) {
        err = newnullout();
    }

    if (!print_sysinc) {
        insertsyspath(&outconf, conf.sys_incpath, conf.delim, 'I');
    }

    if (!print_syslib) {
        insertsyspath(&outconf, conf.sys_libpath, conf.delim, 'L');
    }

    for (Size i = 0; i < nargs; i++) {
        process(a, &proc, args[i]);
    }
    endprocessor(&proc, &err);

    if (!pkgs.count) {
        outstr(&err, S("pkg-config: "));
        outstr(&err, S("requires at least one package name\n"));
        flush(&err);
        os_fail();
    }

    if (modversion) {
        for (Pkg *p = pkgs.head; p; p = p->list) {
            if (p->flags & Pkg_DIRECT) {
                outstr(&out, p->version);
                outstr(&out, S("\n"));
            }
        }
    }

    if (variable.s) {
        for (Pkg *p = pkgs.head; p; p = p->list) {
            if (p->flags & Pkg_DIRECT) {
                Str value = lookup(&global, &p->env, variable);
                if (value.s) {
                    expand(&out, &err, &global, p, value);
                    outstr(&out, S("\n"));
                }
            }
        }
    }

    if (cflags) {
        outconf.filter = filterc;
        for (Pkg *p = pkgs.head; p; p = p->list) {
            fieldout(&outconf, p, p->cflags);
        }
    }

    if (libs) {
        outconf.filter = filterl;
        for (Pkg *p = pkgs.head; p; p = p->list) {
            if (static_) {
                fieldout(&outconf, p, p->libs);
                fieldout(&outconf, p, p->libsprivate);
            } else if (p->flags & Pkg_PUBLIC) {
                fieldout(&outconf, p, p->libs);
            }
        }
    }

    if (cflags || libs) {
        outstr(&out, S("\n"));
    }

    flush(&out);
}
