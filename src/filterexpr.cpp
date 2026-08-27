// Filter expression layer  (Phase 4.3)
//
// Recursive-descent parser producing a small AST, evaluated once per packet.
// See filterexpr.h for the supported grammar and why this exists.
//
// Grammar:
//   or   := and ( ("or" | "||") and )*
//   and  := not ( ("and" | "&&") not )*
//   not  := ("not" | "!") not | primary
//   prim := "(" or ")" | keyword | field op number

#include "filterexpr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

namespace {

// --- node kinds -----------------------------------------------------------
enum NodeKind {
    N_TRUE, N_FALSE,
    N_AND, N_OR, N_NOT,
    N_OUTBOUND, N_INBOUND, N_LOOPBACK,
    N_IPV4, N_IPV6,
    N_CMP                     // field op value
};

// --- comparable packet fields --------------------------------------------
enum Field {
    F_IP_SRC, F_IP_DST, F_IP_PROTO,
    F_TCP_SRCPORT, F_TCP_DSTPORT,
    F_UDP_SRCPORT, F_UDP_DSTPORT
};

enum Op { OP_EQ, OP_NE, OP_GT, OP_LT, OP_GE, OP_LE };

struct Node {
    NodeKind kind;
    int      left  = -1;      // index into FilterProgram::nodes
    int      right = -1;
    Field    field = F_IP_SRC;
    Op       op    = OP_EQ;
    UINT32   value = 0;
    // A protocol keyword implies "this packet is TCP/UDP/ICMP"; those are
    // expressed as an ip.Protocol comparison so there is only one code path.
};

} // namespace

struct FilterProgram {
    std::vector<Node> nodes;
    int root = -1;
};

namespace {

// ---------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------

struct Token {
    enum Kind { End, Ident, Number, IPv4, Punct } kind = End;
    std::string text;
    UINT32      num = 0;
};

class Lexer {
public:
    explicit Lexer(const char *src) : p_(src) {}

    Token next() {
        skipSpace();
        Token t;
        if (!*p_) { t.kind = Token::End; return t; }

        // punctuation / operators
        if (strchr("()!<>=&|", *p_)) {
            t.kind = Token::Punct;
            t.text.push_back(*p_++);
            // two-character forms
            if ((t.text == "!" || t.text == "<" || t.text == ">" || t.text == "=") && *p_ == '=') {
                t.text.push_back(*p_++);
            } else if ((t.text == "&" && *p_ == '&') || (t.text == "|" && *p_ == '|')) {
                t.text.push_back(*p_++);
            }
            return t;
        }

        // number, or a dotted IPv4 literal
        if (*p_ >= '0' && *p_ <= '9') {
            const char *start = p_;
            while ((*p_ >= '0' && *p_ <= '9') || *p_ == '.') p_++;
            t.text.assign(start, p_);
            if (t.text.find('.') != std::string::npos) {
                t.kind = Token::IPv4;
                UINT32 addr = 0;
                if (!parseIPv4(t.text, &addr)) { t.kind = Token::Punct; t.text = "?"; return t; }
                t.num = addr;
            } else {
                t.kind = Token::Number;
                t.num  = (UINT32)strtoul(t.text.c_str(), nullptr, 10);
            }
            return t;
        }

        // identifier: letters, digits, '.', '_'
        if (isIdentStart(*p_)) {
            const char *start = p_;
            while (isIdentChar(*p_)) p_++;
            t.kind = Token::Ident;
            t.text.assign(start, p_);
            return t;
        }

        t.kind = Token::Punct;
        t.text.push_back(*p_++);
        return t;
    }

    static bool parseIPv4(const std::string &s, UINT32 *out) {
        unsigned a, b, c, d;
        char extra;
        if (sscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return false;
        if (a > 255 || b > 255 || c > 255 || d > 255) return false;
        // host byte order; packet fields are converted to match
        *out = (a << 24) | (b << 16) | (c << 8) | d;
        return true;
    }

private:
    void skipSpace() { while (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n') p_++; }
    static bool isIdentStart(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }
    static bool isIdentChar(char c) {
        return isIdentStart(c) || (c >= '0' && c <= '9') || c == '.';
    }
    const char *p_;
};

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

class Parser {
public:
    Parser(const char *src, FilterProgram *prog) : lex_(src), prog_(prog) {
        cur_ = lex_.next();
    }

    bool parse(int *root) {
        const int n = parseOr();
        if (n < 0) return false;
        if (cur_.kind != Token::End) {
            fail("unexpected '" + cur_.text + "'");
            return false;
        }
        *root = n;
        return true;
    }

    const std::string& error() const { return err_; }

private:
    // --- helpers ---
    int add(Node n) {
        prog_->nodes.push_back(n);
        return (int)prog_->nodes.size() - 1;
    }
    void advance() { cur_ = lex_.next(); }
    bool isIdent(const char *kw) const {
        if (cur_.kind != Token::Ident) return false;
        return _stricmp(cur_.text.c_str(), kw) == 0;
    }
    bool isPunct(const char *s) const {
        return cur_.kind == Token::Punct && cur_.text == s;
    }
    int fail(const std::string &msg) {
        if (err_.empty()) err_ = msg;
        return -1;
    }

    int makeProtoNode(UINT32 proto) {
        Node n;
        n.kind  = N_CMP;
        n.field = F_IP_PROTO;
        n.op    = OP_EQ;
        n.value = proto;
        return add(n);
    }

    // --- grammar ---
    int parseOr() {
        int left = parseAnd();
        if (left < 0) return -1;
        while (isIdent("or") || isPunct("||")) {
            advance();
            const int right = parseAnd();
            if (right < 0) return -1;
            Node n; n.kind = N_OR; n.left = left; n.right = right;
            left = add(n);
        }
        return left;
    }

    int parseAnd() {
        int left = parseNot();
        if (left < 0) return -1;
        while (isIdent("and") || isPunct("&&")) {
            advance();
            const int right = parseNot();
            if (right < 0) return -1;
            Node n; n.kind = N_AND; n.left = left; n.right = right;
            left = add(n);
        }
        return left;
    }

    int parseNot() {
        if (isIdent("not") || isPunct("!")) {
            advance();
            const int operand = parseNot();
            if (operand < 0) return -1;
            Node n; n.kind = N_NOT; n.left = operand;
            return add(n);
        }
        return parsePrimary();
    }

    int parsePrimary() {
        if (isPunct("(")) {
            advance();
            const int inner = parseOr();
            if (inner < 0) return -1;
            if (!isPunct(")")) return fail("expected ')'");
            advance();
            return inner;
        }

        if (cur_.kind != Token::Ident) {
            return fail(cur_.kind == Token::End
                        ? "unexpected end of expression"
                        : "expected a keyword or field, got '" + cur_.text + "'");
        }

        const std::string word = cur_.text;

        // --- bare keywords ---
        struct { const char *kw; NodeKind kind; } keywords[] = {
            { "true",     N_TRUE },     { "false",    N_FALSE },
            { "outbound", N_OUTBOUND }, { "inbound",  N_INBOUND },
            { "loopback", N_LOOPBACK }, { "ip",       N_IPV4 },
            { "ipv6",     N_IPV6 },
        };
        for (const auto &k : keywords) {
            if (_stricmp(word.c_str(), k.kw) == 0) {
                advance();
                Node n; n.kind = k.kind;
                return add(n);
            }
        }
        // protocol keywords are ip.Protocol comparisons underneath
        struct { const char *kw; UINT32 proto; } protos[] = {
            { "tcp", 6 }, { "udp", 17 }, { "icmp", 1 }, { "icmpv6", 58 },
        };
        for (const auto &pr : protos) {
            if (_stricmp(word.c_str(), pr.kw) == 0) {
                advance();
                return makeProtoNode(pr.proto);
            }
        }

        // --- field comparison ---
        Field field;
        if (!lookupField(word, &field)) {
            return fail("unknown filter term '" + word + "'");
        }
        advance();

        Op op;
        if (!lookupOp(cur_.text, &op) || cur_.kind != Token::Punct) {
            return fail("expected a comparison operator after '" + word + "'");
        }
        advance();

        if (cur_.kind != Token::Number && cur_.kind != Token::IPv4) {
            return fail("expected a value after '" + word + "'");
        }
        const bool wantsAddr = (field == F_IP_SRC || field == F_IP_DST);
        if (wantsAddr && cur_.kind != Token::IPv4) {
            return fail("'" + word + "' needs a dotted IPv4 address");
        }
        if (!wantsAddr && cur_.kind == Token::IPv4) {
            return fail("'" + word + "' needs a number, not an address");
        }

        Node n;
        n.kind  = N_CMP;
        n.field = field;
        n.op    = op;
        n.value = cur_.num;
        advance();
        return add(n);
    }

    static bool lookupField(const std::string &w, Field *out) {
        struct { const char *name; Field f; } table[] = {
            { "ip.SrcAddr",  F_IP_SRC },      { "ip.DstAddr",  F_IP_DST },
            { "ip.Protocol", F_IP_PROTO },
            { "tcp.SrcPort", F_TCP_SRCPORT }, { "tcp.DstPort", F_TCP_DSTPORT },
            { "udp.SrcPort", F_UDP_SRCPORT }, { "udp.DstPort", F_UDP_DSTPORT },
        };
        for (const auto &e : table) {
            if (_stricmp(w.c_str(), e.name) == 0) { *out = e.f; return true; }
        }
        return false;
    }

    static bool lookupOp(const std::string &s, Op *out) {
        if (s == "==" || s == "=") { *out = OP_EQ; return true; }
        if (s == "!=")             { *out = OP_NE; return true; }
        if (s == ">=")             { *out = OP_GE; return true; }
        if (s == "<=")             { *out = OP_LE; return true; }
        if (s == ">")              { *out = OP_GT; return true; }
        if (s == "<")              { *out = OP_LT; return true; }
        return false;
    }

    Lexer          lex_;
    Token          cur_;
    FilterProgram *prog_;
    std::string    err_;
};

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

// What we can read out of one packet, extracted once per evaluation.
struct PacketFields {
    int    valid      = 0;
    int    isV4       = 0;
    UINT32 srcAddr    = 0;   // host byte order, IPv4 only
    UINT32 dstAddr    = 0;
    UINT32 protocol   = 0;
    int    hasPorts   = 0;
    UINT32 srcPort    = 0;
    UINT32 dstPort    = 0;
};

UINT16 read16be(const unsigned char *p) {
    return (UINT16)((p[0] << 8) | p[1]);
}
UINT32 read32be(const unsigned char *p) {
    return ((UINT32)p[0] << 24) | ((UINT32)p[1] << 16) |
           ((UINT32)p[2] << 8)  | (UINT32)p[3];
}

void extractFields(const char *packet, UINT len, PacketFields *f) {
    const unsigned char *p = (const unsigned char*)packet;
    if (len < 20) return;

    const unsigned version = p[0] >> 4;
    unsigned transportOffset;

    if (version == 4) {
        const unsigned ihl = (p[0] & 0x0F) * 4;
        if (ihl < 20 || len < ihl) return;
        f->isV4     = 1;
        f->protocol = p[9];
        f->srcAddr  = read32be(p + 12);
        f->dstAddr  = read32be(p + 16);
        transportOffset = ihl;
        // A fragment other than the first has no transport header to read.
        if ((read16be(p + 6) & 0x1FFF) != 0) {
            f->valid = 1;
            return;
        }
    } else if (version == 6) {
        if (len < 40) return;
        f->isV4     = 0;
        f->protocol = p[6];          // next header; extension headers not walked
        transportOffset = 40;
    } else {
        return;
    }

    if ((f->protocol == 6 || f->protocol == 17) && len >= transportOffset + 4) {
        f->hasPorts = 1;
        f->srcPort  = read16be(p + transportOffset);
        f->dstPort  = read16be(p + transportOffset + 2);
    }
    f->valid = 1;
}

int compare(UINT32 lhs, Op op, UINT32 rhs) {
    switch (op) {
    case OP_NE: return lhs != rhs;
    case OP_GT: return lhs >  rhs;
    case OP_LT: return lhs <  rhs;
    case OP_GE: return lhs >= rhs;
    case OP_LE: return lhs <= rhs;
    default:    return lhs == rhs;
    }
}

int evalNode(const FilterProgram *prog, int idx, const PacketMeta *meta,
             const PacketFields *f) {
    if (idx < 0 || idx >= (int)prog->nodes.size()) return 0;
    const Node &n = prog->nodes[(size_t)idx];

    switch (n.kind) {
    case N_TRUE:     return 1;
    case N_FALSE:    return 0;
    case N_AND:      return evalNode(prog, n.left, meta, f) &&
                            evalNode(prog, n.right, meta, f);
    case N_OR:       return evalNode(prog, n.left, meta, f) ||
                            evalNode(prog, n.right, meta, f);
    case N_NOT:      return !evalNode(prog, n.left, meta, f);
    case N_OUTBOUND: return meta->outbound ? 1 : 0;
    case N_INBOUND:  return meta->outbound ? 0 : 1;
    case N_LOOPBACK: return meta->loopback ? 1 : 0;
    case N_IPV4:     return f->valid && f->isV4;
    case N_IPV6:     return f->valid && !f->isV4;
    case N_CMP:
        if (!f->valid) return 0;
        switch (n.field) {
        case F_IP_SRC:
            return f->isV4 && compare(f->srcAddr, n.op, n.value);
        case F_IP_DST:
            return f->isV4 && compare(f->dstAddr, n.op, n.value);
        case F_IP_PROTO:
            return compare(f->protocol, n.op, n.value);
        case F_TCP_SRCPORT:
            return f->hasPorts && f->protocol == 6 && compare(f->srcPort, n.op, n.value);
        case F_TCP_DSTPORT:
            return f->hasPorts && f->protocol == 6 && compare(f->dstPort, n.op, n.value);
        case F_UDP_SRCPORT:
            return f->hasPorts && f->protocol == 17 && compare(f->srcPort, n.op, n.value);
        case F_UDP_DSTPORT:
            return f->hasPorts && f->protocol == 17 && compare(f->dstPort, n.op, n.value);
        }
        return 0;
    }
    return 0;
}

int isBlank(const char *s) {
    if (!s) return 1;
    for (; *s; ++s) {
        if (*s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') return 0;
    }
    return 1;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

FilterProgram* filterCompile(const char *expr, char *errBuf, int errSize) {
    FilterProgram *prog = new FilterProgram();

    if (isBlank(expr)) {
        Node n; n.kind = N_TRUE;
        prog->nodes.push_back(n);
        prog->root = 0;
        return prog;
    }

    Parser parser(expr, prog);
    if (!parser.parse(&prog->root)) {
        if (errBuf && errSize > 0) {
            snprintf(errBuf, (size_t)errSize, "filter syntax error: %s",
                     parser.error().empty() ? "invalid expression" : parser.error().c_str());
        }
        delete prog;
        return nullptr;
    }
    return prog;
}

int filterMatch(const FilterProgram *prog, const PacketMeta *meta,
                const char *packet, UINT len) {
    if (!prog || prog->root < 0) return 1;
    PacketFields fields;
    extractFields(packet, len, &fields);
    return evalNode(prog, prog->root, meta, &fields);
}

void filterFree(FilterProgram *prog) {
    delete prog;
}
