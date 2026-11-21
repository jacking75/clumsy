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
    N_CMP,                    // field op value
    N_OPAQUE                  // lenient parse only: a term outside the subset
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
    int      atom  = -1;      // N_OPAQUE: index into FilterProgram::atoms
    // A protocol keyword implies "this packet is TCP/UDP/ICMP"; those are
    // expressed as an ip.Protocol comparison so there is only one code path.
};

} // namespace

struct FilterProgram {
    std::vector<Node> nodes;
    int root = -1;
    std::vector<std::string> atoms;   // N_OPAQUE texts, normalised
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

    // The value after a comparison operator, taken verbatim up to the next
    // space, parenthesis or boolean operator. Used by the lenient parse, where
    // a value may be anything WinDivert accepts: hex, an IPv6 address, ...
    std::string rawValue() {
        skipSpace();
        const char *start = p_;
        while (*p_ && !strchr(" \t\r\n()&|!", *p_)) p_++;
        return std::string(start, p_);
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
    Parser(const char *src, FilterProgram *prog, bool lenient)
        : lex_(src), prog_(prog), lenient_(lenient) {
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

        if (lenient_) return parseLenientTerm(word);

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

    // Lenient parse: any identifier is a term. A known field whose value reads
    // as a number (decimal or 0x hex) or a dotted IPv4 address becomes a real
    // comparison; everything else is kept as an opaque condition named by its
    // own normalised text, so the same term in two filters is the same
    // condition.
    int parseLenientTerm(const std::string &word) {
        std::string key = lower(word);
        advance();

        Op op;
        if (cur_.kind == Token::Punct && lookupOp(cur_.text, &op)) {
            const std::string opText = (cur_.text == "=") ? "==" : cur_.text;
            const std::string value  = lex_.rawValue();
            advance();
            if (value.empty()) return fail("expected a value after '" + word + "'");

            Field field;
            UINT32 num;
            if (lookupField(word, &field) && parseValue(field, value, &num)) {
                Node n;
                n.kind  = N_CMP;
                n.field = field;
                n.op    = op;
                n.value = num;
                return add(n);
            }
            key += " " + opText + " " + lower(value);
        }

        Node n;
        n.kind = N_OPAQUE;
        n.atom = (int)prog_->atoms.size();
        prog_->atoms.push_back(key);
        return add(n);
    }

    static bool parseValue(Field field, const std::string &text, UINT32 *out) {
        if (field == F_IP_SRC || field == F_IP_DST) return Lexer::parseIPv4(text, out);
        char *end = nullptr;
        const unsigned long long v = strtoull(text.c_str(), &end, 0);
        if (!end || *end != '\0' || v > 0xFFFFFFFFull) return false;
        *out = (UINT32)v;
        return true;
    }

    static std::string lower(const std::string &s) {
        std::string r = s;
        for (char &c : r) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return r;
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
    bool           lenient_;
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
    case N_OPAQUE:           // only produced by the lenient parse, never evaluated
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

namespace {

FilterProgram* compileWith(const char *expr, bool lenient, char *errBuf, int errSize) {
    FilterProgram *prog = new FilterProgram();

    if (isBlank(expr)) {
        Node n; n.kind = N_TRUE;
        prog->nodes.push_back(n);
        prog->root = 0;
        return prog;
    }

    Parser parser(expr, prog, lenient);
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

} // namespace

FilterProgram* filterCompile(const char *expr, char *errBuf, int errSize) {
    return compileWith(expr, false, errBuf, errSize);
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

// ---------------------------------------------------------------------------
// iptables rule derivation
// ---------------------------------------------------------------------------

namespace {

// Constraints collected from one AND-chain. Anything left at its default means
// "unconstrained", which widens the rule - the safe direction.
struct Constraints {
    int      chains   = IPT_CHAIN_OUTPUT | IPT_CHAIN_INPUT;
    int      proto    = 0;      // 0 = any, else IP protocol number
    int      ipv6     = -1;     // -1 unknown, 0 = v4, 1 = v6
    long     dstPort  = -1;
    long     srcPort  = -1;
    UINT32   dstAddr  = 0;      // 0 = unconstrained
    UINT32   srcAddr  = 0;
    bool     widened  = false;  // something could not be expressed
};

// Walks one AND-chain, folding every term it understands into c.
// Terms under a NOT, or any comparison iptables cannot express as an equality,
// only mark the result as widened.
void collect(const FilterProgram *prog, int idx, Constraints &c, bool negated) {
    if (idx < 0 || idx >= (int)prog->nodes.size()) return;
    const Node &n = prog->nodes[(size_t)idx];

    switch (n.kind) {
    case N_AND:
        collect(prog, n.left,  c, negated);
        collect(prog, n.right, c, negated);
        return;
    case N_NOT:
        // A negated term cannot narrow an iptables rule safely; note it and
        // keep the rule as wide as it already is.
        c.widened = true;
        collect(prog, n.left, c, !negated);
        return;
    case N_OR:
        // Handled by splitting before we get here; reaching it means a nested
        // OR that we choose not to expand.
        c.widened = true;
        return;
    case N_TRUE:
        return;
    case N_FALSE:
    case N_OPAQUE:
        c.widened = true;
        return;
    case N_OUTBOUND:
        if (!negated) c.chains = IPT_CHAIN_OUTPUT;
        return;
    case N_INBOUND:
        if (!negated) c.chains = IPT_CHAIN_INPUT;
        return;
    case N_LOOPBACK:
        // -i lo / -o lo would work, but combining it with the chain split adds
        // little: loopback traffic already matches the generated rules.
        c.widened = true;
        return;
    case N_IPV4:
        if (!negated) c.ipv6 = 0;
        return;
    case N_IPV6:
        if (!negated) c.ipv6 = 1;
        return;
    case N_CMP:
        if (negated || n.op != OP_EQ) { c.widened = true; return; }
        switch (n.field) {
        case F_IP_PROTO:
            c.proto = (int)n.value;
            // Some protocol numbers only exist on one side of the family:
            // ICMP (1) is IPv4, ICMPv6 (58) is IPv6. Pinning the version here
            // is what stops "--filter icmpv6" from producing an
            // "iptables -p 58" rule that matches nothing on IPv4 while the
            // real ICMPv6 traffic never reaches the queue at all.
            if (c.proto == 1)  c.ipv6 = 0;
            if (c.proto == 58) c.ipv6 = 1;
            break;
        case F_TCP_SRCPORT:  c.proto = 6;  c.srcPort = (long)n.value; break;
        case F_TCP_DSTPORT:  c.proto = 6;  c.dstPort = (long)n.value; break;
        case F_UDP_SRCPORT:  c.proto = 17; c.srcPort = (long)n.value; break;
        case F_UDP_DSTPORT:  c.proto = 17; c.dstPort = (long)n.value; break;
        case F_IP_SRC:       c.srcAddr = n.value; c.ipv6 = 0; break;
        case F_IP_DST:       c.dstAddr = n.value; c.ipv6 = 0; break;
        }
        return;
    }
}

// Hard stop on pathological nesting. Well above MAX_DERIVED_RULES on purpose:
// the caller needs to be able to tell "eight branches" from "more branches than
// there is room for", and the old code could not, because it stopped splitting
// once `out` was full and then quietly dropped whatever was left over.
#define SPLIT_BRANCH_LIMIT 64

// Splits the top level on OR so each branch becomes its own rule; an OR of two
// ports is extremely common ("port 7777 in either direction") and one rule per
// branch keeps those precise.
void splitOr(const FilterProgram *prog, int idx, std::vector<int> &out, int depth) {
    if (idx < 0 || idx >= (int)prog->nodes.size()) return;
    if (out.size() >= SPLIT_BRANCH_LIMIT) return;
    const Node &n = prog->nodes[(size_t)idx];
    if (n.kind == N_OR && depth < 16) {
        splitOr(prog, n.left,  out, depth + 1);
        splitOr(prog, n.right, out, depth + 1);
        return;
    }
    out.push_back(idx);
}

void formatAddr(UINT32 addr, char *buf, size_t bufSize) {
    snprintf(buf, bufSize, "%u.%u.%u.%u",
             (addr >> 24) & 0xFF, (addr >> 16) & 0xFF,
             (addr >> 8) & 0xFF, addr & 0xFF);
}

// ipv6 selects the spelling iptables and ip6tables disagree on; everything
// else is identical between the two.
void buildMatch(const Constraints &c, int ipv6, char *out, size_t outSize) {
    char buf[256];
    const int size = (int)sizeof(buf);
    int pos = 0;
    buf[0] = '\0';

    if (c.proto == 6)                    pos = appendf(buf, size, pos, "-p tcp ");
    else if (c.proto == 17)              pos = appendf(buf, size, pos, "-p udp ");
    else if (c.proto == 1 && !ipv6)      pos = appendf(buf, size, pos, "-p icmp ");
    else if (c.proto == 58 && ipv6)      pos = appendf(buf, size, pos, "-p icmpv6 ");
    else if (c.proto > 0)                pos = appendf(buf, size, pos, "-p %d ", c.proto);

    // Ports need a protocol; without one iptables rejects --dport.
    if (c.proto == 6 || c.proto == 17) {
        if (c.dstPort >= 0) pos = appendf(buf, size, pos, "--dport %ld ", c.dstPort);
        if (c.srcPort >= 0) pos = appendf(buf, size, pos, "--sport %ld ", c.srcPort);
    }
    // Address literals are dotted IPv4 only, and collect() pins ipv6 = 0 the
    // moment it sees one, so these never reach an ip6tables rule.
    if (c.srcAddr) {
        char a[32]; formatAddr(c.srcAddr, a, sizeof(a));
        pos = appendf(buf, size, pos, "-s %s ", a);
    }
    if (c.dstAddr) {
        char a[32]; formatAddr(c.dstAddr, a, sizeof(a));
        pos = appendf(buf, size, pos, "-d %s ", a);
    }

    // Trim the trailing space so the assembled command line stays tidy.
    while (pos > 0 && buf[pos - 1] == ' ') buf[--pos] = '\0';
    snprintf(out, outSize, "%s", buf);
}

// The same rule spelled as a filter expression: exactly the packets buildMatch()
// on these chains lets into the queue.
void buildExpr(const Constraints &c, int ipv6, char *out, size_t outSize) {
    char buf[256];
    const int size = (int)sizeof(buf);
    int pos = appendf(buf, size, 0, "%s", ipv6 ? "ipv6" : "ip");

    if (c.chains == IPT_CHAIN_OUTPUT)     pos = appendf(buf, size, pos, " and outbound");
    else if (c.chains == IPT_CHAIN_INPUT) pos = appendf(buf, size, pos, " and inbound");

    if (c.proto > 0) pos = appendf(buf, size, pos, " and ip.Protocol == %d", c.proto);
    if (c.proto == 6 || c.proto == 17) {
        const char *p = (c.proto == 6) ? "tcp" : "udp";
        if (c.dstPort >= 0) pos = appendf(buf, size, pos, " and %s.DstPort == %ld", p, c.dstPort);
        if (c.srcPort >= 0) pos = appendf(buf, size, pos, " and %s.SrcPort == %ld", p, c.srcPort);
    }
    if (c.srcAddr) {
        char a[32]; formatAddr(c.srcAddr, a, sizeof(a));
        pos = appendf(buf, size, pos, " and ip.SrcAddr == %s", a);
    }
    if (c.dstAddr) {
        char a[32]; formatAddr(c.dstAddr, a, sizeof(a));
        pos = appendf(buf, size, pos, " and ip.DstAddr == %s", a);
    }
    snprintf(out, outSize, "%s", buf);
}

// Turns one AND-chain into the rule(s) it needs, appended to out.
void emitRules(const FilterProgram *prog, int branch,
               std::vector<IptablesRule> &out, int *exact) {
    Constraints c;
    collect(prog, branch, c, false);
    if (c.widened && exact) *exact = 0;

    // An expression that never pins the IP version has to produce rules for
    // both. "Unknown" silently becoming "IPv4" was the one place this
    // translation narrowed instead of widening, and it still reported the
    // result as exact - so running with no --filter at all installed iptables
    // rules only, and every IPv6 packet on the host passed by untouched.
    const bool wantV4 = (c.ipv6 != 1);
    const bool wantV6 = (c.ipv6 != 0);

    for (int v6 = 0; v6 <= 1; ++v6) {
        IptablesRule r;
        if (v6 == 0 && !wantV4) continue;
        if (v6 == 1 && !wantV6) continue;
        r.chains = c.chains;
        r.ipv6   = v6;
        buildMatch(c, v6, r.match, sizeof(r.match));
        buildExpr(c, v6, r.expr, sizeof(r.expr));
        out.push_back(r);
    }
}

// An OR of two ports on the same protocol often collapses to identical match
// strings once direction is folded in.
void dedupeRules(std::vector<IptablesRule> &v) {
    std::vector<IptablesRule> uniq;
    for (size_t i = 0; i < v.size(); ++i) {
        bool dup = false;
        for (size_t j = 0; j < uniq.size(); ++j) {
            if (uniq[j].chains == v[i].chains && uniq[j].ipv6 == v[i].ipv6 &&
                strcmp(uniq[j].match, v[i].match) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) uniq.push_back(v[i]);
    }
    v.swap(uniq);
}

} // namespace

int filterDeriveIptables(const FilterProgram *prog, IptablesRule *rules,
                         int maxRules, int *exact) {
    int localExact = 1;
    int count = 0;

    if (exact) *exact = 1;
    if (!prog || !rules || maxRules <= 0) return 0;

    std::vector<int> branches;
    if (prog->root >= 0) splitOr(prog, prog->root, branches, 0);
    if (branches.empty()) branches.push_back(prog->root);

    std::vector<IptablesRule> derived;
    for (size_t i = 0; i < branches.size(); ++i) {
        emitRules(prog, branches[i], derived, &localExact);
    }
    dedupeRules(derived);

    if (derived.size() > (size_t)maxRules) {
        // More rules than there is room for. Emitting the first maxRules and
        // dropping the rest would *narrow* the result, and this file's whole
        // contract is that derived rules are a superset: under-capturing
        // silently misses the traffic under test while still reporting
        // success. Fall back to a single derivation over the un-split
        // expression - collect() cannot express a top-level OR, so it widens
        // to "everything", which is safe and is flagged as inexact.
        INFO("auto-iptables: the filter needs %d rules but only %d fit; "
             "falling back to one wide rule and letting clumsy's own filter "
             "do the selecting.", (int)derived.size(), maxRules);
        derived.clear();
        localExact = 0;
        emitRules(prog, prog->root, derived, NULL);
        dedupeRules(derived);
    }

    for (size_t i = 0; i < derived.size() && count < maxRules; ++i) {
        rules[count++] = derived[i];
    }
    if (exact) *exact = localExact;
    return count;
}

int filterCaptureExpr(const char *filter, char *out, int outSize) {
    IptablesRule rules[MAX_DERIVED_RULES];
    char err[MSG_BUFSIZE];
    int exact = 1, pos = 0;

    FilterProgram *prog = filterCompile(filter, err, sizeof(err));
    if (!prog) return 0;
    const int count = filterDeriveIptables(prog, rules, MAX_DERIVED_RULES, &exact);
    filterFree(prog);
    if (count <= 0 || outSize <= 0) return 0;

    out[0] = '\0';
    for (int i = 0; i < count; ++i) {
        pos = appendf(out, outSize, pos, "%s(%s)", i ? " or " : "", rules[i].expr);
    }
    return pos < outSize - 1;
}

// ---------------------------------------------------------------------------
// Overlap analysis
//
// Is "A and B" satisfiable by some packet? The search is a tableau: terms
// that only narrow the packet (an AND, a direction, a port equality) are folded
// into a model at once, and each OR is a branch point tried one alternative at
// a time. A branch dies as soon as the model becomes impossible - a port that
// must be both 7777 and 50001, a packet both inbound and outbound - so the
// usual "this port or that port" filters cost a few hundred steps.
//
// The model mirrors evalNode(): ports exist only on TCP/UDP packets that carry
// a transport header, addresses only on IPv4. A negated port test is therefore
// true for a packet with no ports at all, which is why it branches three ways.
// ---------------------------------------------------------------------------

namespace {

typedef unsigned long long u64;

// The values one numeric packet field can still take: [lo, hi] minus ne.
struct Range {
    u64 lo, hi;
    std::vector<u64> ne;
};

bool rangeHasValue(const Range &r) {
    if (r.lo > r.hi) return false;
    std::vector<u64> inside;
    for (u64 v : r.ne) {
        if (v < r.lo || v > r.hi) continue;
        bool seen = false;
        for (u64 w : inside) if (w == v) { seen = true; break; }
        if (!seen) inside.push_back(v);
    }
    return (u64)inside.size() < r.hi - r.lo + 1;
}

// Smallest value still allowed. Only called on a range that has one.
u64 rangePick(const Range &r) {
    for (u64 v = r.lo; ; ++v) {
        bool excluded = false;
        for (u64 w : r.ne) if (w == v) { excluded = true; break; }
        if (!excluded) return v;
    }
}

bool rangeNarrowed(const Range &r, u64 max) {
    return r.lo != 0 || r.hi != max || !r.ne.empty();
}

bool rangeApply(Range &r, Op op, u64 v, u64 max) {
    u64 lo = 0, hi = max;          // the values this one comparison allows
    switch (op) {
    case OP_EQ: lo = v; hi = v; break;
    case OP_NE: r.ne.push_back(v); break;
    case OP_GT:
        if (v >= max) return false;
        lo = v + 1;
        break;
    case OP_LT:
        if (v == 0) return false;
        hi = v - 1;
        break;
    case OP_GE: lo = v; break;
    case OP_LE: hi = v; break;
    }
    if (lo > r.lo) r.lo = lo;
    if (hi < r.hi) r.hi = hi;
    return rangeHasValue(r);
}

Op negateOp(Op op) {
    switch (op) {
    case OP_EQ: return OP_NE;
    case OP_NE: return OP_EQ;
    case OP_GT: return OP_LE;
    case OP_LT: return OP_GE;
    case OP_GE: return OP_LT;
    case OP_LE: return OP_GT;
    }
    return OP_EQ;
}

const u64 MAX_PROTO = 255, MAX_PORT = 65535, MAX_ADDR = 0xFFFFFFFFull;

// What is known so far about the one packet both filters must match.
// Flags are -1 unknown, 0 false, 1 true.
struct Model {
    signed char outbound = -1, loopback = -1, v4 = -1, ports = -1;
    Range proto { 0, MAX_PROTO, {} };
    Range sport { 0, MAX_PORT,  {} };
    Range dport { 0, MAX_PORT,  {} };
    Range saddr { 0, MAX_ADDR,  {} };
    Range daddr { 0, MAX_ADDR,  {} };
    std::vector<signed char> opaque;     // by global atom id
};

bool setFlag(signed char &flag, bool value) {
    if (flag < 0) { flag = value ? 1 : 0; return true; }
    return flag == (value ? 1 : 0);
}

struct Term {
    const FilterProgram *prog;
    int  node;
    bool pos;                 // false: the term must be false
};

// One alternative at a branch point: either a term to make true or false, or
// one of the ways a negated address/port test can come out false.
enum AltKind { ALT_TERM, ALT_NO_FIELD, ALT_OTHER_PROTO, ALT_CMP_FALSE };
struct Alt {
    AltKind kind;
    Term    term;
};

class OverlapSearch {
public:
    OverlapSearch(const FilterProgram *a, const FilterProgram *b) : a_(a), b_(b) {
        mapAtoms(a, idsA_);
        mapAtoms(b, idsB_);
    }

    // 1 = some packet matches both (model in *out), 0 = none, -1 = gave up.
    int run(Model *out) {
        std::vector<Term> todo;
        todo.push_back({ a_, a_->root, true });
        todo.push_back({ b_, b_->root, true });
        Model m;
        m.opaque.assign(names_.size(), -1);
        if (solve(todo, m, out)) return 1;
        return aborted_ ? -1 : 0;
    }

    const std::string& atomName(int id) const { return names_[(size_t)id]; }

private:
    void mapAtoms(const FilterProgram *p, std::vector<int> &ids) {
        for (const std::string &s : p->atoms) {
            int id = -1;
            for (size_t i = 0; i < names_.size(); ++i) {
                if (names_[i] == s) { id = (int)i; break; }
            }
            if (id < 0) { id = (int)names_.size(); names_.push_back(s); }
            ids.push_back(id);
        }
    }

    int atomId(const Term &t) const {
        const Node &n = t.prog->nodes[(size_t)t.node];
        return (t.prog == a_ ? idsA_ : idsB_)[(size_t)n.atom];
    }

    static const Node& node(const Term &t) { return t.prog->nodes[(size_t)t.node]; }

    static bool isAddrField(Field f) { return f == F_IP_SRC || f == F_IP_DST; }
    static bool isPortField(Field f) { return f != F_IP_PROTO && !isAddrField(f); }

    static bool branches(const Term &t) {
        const Node &n = node(t);
        if (n.kind == N_OR)  return t.pos;
        if (n.kind == N_AND) return !t.pos;
        if (n.kind == N_CMP) return !t.pos && n.field != F_IP_PROTO;
        return false;
    }

    // Folds a non-branching term into the model; AND-like terms push their
    // children onto todo instead. Returns false when the model breaks.
    bool fold(const Term &t, std::vector<Term> &todo, Model &m) {
        const Node &n = node(t);
        switch (n.kind) {
        case N_TRUE:     return t.pos;
        case N_FALSE:    return !t.pos;
        case N_NOT:      todo.push_back({ t.prog, n.left, !t.pos }); return true;
        case N_AND:
        case N_OR:
            todo.push_back({ t.prog, n.left,  t.pos });
            todo.push_back({ t.prog, n.right, t.pos });
            return true;
        case N_OUTBOUND: return setFlag(m.outbound, t.pos);
        case N_INBOUND:  return setFlag(m.outbound, !t.pos);
        case N_LOOPBACK: return setFlag(m.loopback, t.pos);
        case N_IPV4:     return setFlag(m.v4, t.pos);
        case N_IPV6:     return setFlag(m.v4, !t.pos);
        case N_OPAQUE:   return setFlag(m.opaque[(size_t)atomId(t)], t.pos);
        case N_CMP:
            if (n.field == F_IP_PROTO) {
                return rangeApply(m.proto, t.pos ? n.op : negateOp(n.op), n.value, MAX_PROTO);
            }
            return guardTrue(n, m) && cmpApply(n, n.op, m);   // positive only
        }
        return false;
    }

    // The packet has the field this comparison reads.
    static bool guardTrue(const Node &n, Model &m) {
        if (isAddrField(n.field)) return setFlag(m.v4, true);
        const u64 proto = (n.field == F_TCP_SRCPORT || n.field == F_TCP_DSTPORT) ? 6 : 17;
        return setFlag(m.ports, true) && rangeApply(m.proto, OP_EQ, proto, MAX_PROTO);
    }

    static bool cmpApply(const Node &n, Op op, Model &m) {
        switch (n.field) {
        case F_IP_SRC:      return rangeApply(m.saddr, op, n.value, MAX_ADDR);
        case F_IP_DST:      return rangeApply(m.daddr, op, n.value, MAX_ADDR);
        case F_TCP_SRCPORT:
        case F_UDP_SRCPORT: return rangeApply(m.sport, op, n.value, MAX_PORT);
        case F_TCP_DSTPORT:
        case F_UDP_DSTPORT: return rangeApply(m.dport, op, n.value, MAX_PORT);
        case F_IP_PROTO:    return rangeApply(m.proto, op, n.value, MAX_PROTO);
        }
        return false;
    }

    // Every way a branching term can be satisfied. Nested ORs (or negated
    // ANDs) are flattened so "port a or port b or ..." is one branch point.
    static void alternatives(const Term &t, std::vector<Alt> &out) {
        const Node &n = node(t);
        if (n.kind == N_CMP) {
            out.push_back({ ALT_NO_FIELD, t });
            if (isPortField(n.field)) out.push_back({ ALT_OTHER_PROTO, t });
            out.push_back({ ALT_CMP_FALSE, t });
            return;
        }
        const NodeKind joint = n.kind;
        std::vector<int> stack { t.node };
        while (!stack.empty()) {
            const int idx = stack.back();
            stack.pop_back();
            const Node &c = t.prog->nodes[(size_t)idx];
            if (c.kind == joint) {
                stack.push_back(c.right);
                stack.push_back(c.left);
            } else {
                out.push_back({ ALT_TERM, { t.prog, idx, t.pos } });
            }
        }
    }

    static bool applyAlt(const Alt &alt, std::vector<Term> &todo, Model &m) {
        const Node &n = node(alt.term);
        switch (alt.kind) {
        case ALT_TERM:
            todo.push_back(alt.term);
            return true;
        case ALT_NO_FIELD:
            return isAddrField(n.field) ? setFlag(m.v4, false) : setFlag(m.ports, false);
        case ALT_OTHER_PROTO: {
            const u64 proto = (n.field == F_TCP_SRCPORT || n.field == F_TCP_DSTPORT) ? 6 : 17;
            return rangeApply(m.proto, OP_NE, proto, MAX_PROTO);
        }
        case ALT_CMP_FALSE:
            return cmpApply(n, negateOp(n.op), m);
        }
        return false;
    }

    bool solve(std::vector<Term> todo, Model m, Model *out) {
        if (--budget_ < 0) { aborted_ = true; return false; }

        // Fold everything that does not branch, repeatedly: folding an AND can
        // expose more.
        for (size_t i = 0; i < todo.size(); ) {
            if (branches(todo[i])) { ++i; continue; }
            const Term t = todo[i];
            todo[i] = todo.back();
            todo.pop_back();
            if (!fold(t, todo, m)) return false;
            i = 0;
        }
        if (todo.empty()) { *out = m; return true; }

        // Branch on the point with the fewest alternatives.
        size_t pick = 0;
        std::vector<Alt> alts;
        for (size_t i = 0; i < todo.size(); ++i) {
            std::vector<Alt> cand;
            alternatives(todo[i], cand);
            if (alts.empty() || cand.size() < alts.size()) { alts.swap(cand); pick = i; }
        }
        todo[pick] = todo.back();
        todo.pop_back();

        for (const Alt &alt : alts) {
            std::vector<Term> nextTodo = todo;
            Model nextModel = m;
            if (applyAlt(alt, nextTodo, nextModel) && solve(nextTodo, nextModel, out)) return true;
            if (aborted_) return false;
        }
        return false;
    }

    const FilterProgram *a_, *b_;
    std::vector<int> idsA_, idsB_;
    std::vector<std::string> names_;
    // Search steps before giving up. Two --process filters of 64 ports each
    // need about 70 000; past this the answer would take long enough to stall
    // the caller, which holds the instance registry lock.
    long budget_ = 1000000;
    bool aborted_ = false;
};

// "outbound, IPv4, udp, src port 50001, dst port 7777" - an example packet the
// model allows, naming only what the filters actually pinned down.
void describeModel(const Model &m, const OverlapSearch &s, char *out, int outSize) {
    int pos = 0;
    out[0] = '\0';
#define PART(...) (pos = appendf(out, outSize, pos, "%s", pos ? ", " : ""), \
                   pos = appendf(out, outSize, pos, __VA_ARGS__))

    if (m.outbound >= 0) PART("%s", m.outbound ? "outbound" : "inbound");
    if (m.loopback >= 0) PART("%s", m.loopback ? "loopback" : "not loopback");
    if (m.v4 >= 0)       PART("%s", m.v4 ? "IPv4" : "IPv6");
    if (rangeNarrowed(m.proto, MAX_PROTO)) {
        const u64 p = rangePick(m.proto);
        if (p == 6)       PART("tcp");
        else if (p == 17) PART("udp");
        else if (p == 1)  PART("icmp");
        else if (p == 58) PART("icmpv6");
        else              PART("ip.Protocol %llu", p);
    }
    if (m.ports != 0) {
        if (rangeNarrowed(m.sport, MAX_PORT)) PART("src port %llu", rangePick(m.sport));
        if (rangeNarrowed(m.dport, MAX_PORT)) PART("dst port %llu", rangePick(m.dport));
    }
    if (m.v4 != 0) {
        if (rangeNarrowed(m.saddr, MAX_ADDR)) {
            char a[32]; formatAddr((UINT32)rangePick(m.saddr), a, sizeof(a));
            PART("src %s", a);
        }
        if (rangeNarrowed(m.daddr, MAX_ADDR)) {
            char a[32]; formatAddr((UINT32)rangePick(m.daddr), a, sizeof(a));
            PART("dst %s", a);
        }
    }
    for (size_t i = 0; i < m.opaque.size(); ++i) {
        if (m.opaque[i] >= 0) {
            PART("%s%s", m.opaque[i] ? "" : "not ", s.atomName((int)i).c_str());
        }
    }
    if (!pos) PART("any packet");
#undef PART
}

} // namespace

int filterOverlap(const char *exprA, const char *exprB, char *msg, int msgSize) {
    char err[MSG_BUFSIZE];
    int result;

    if (msg && msgSize > 0) msg[0] = '\0';

    FilterProgram *a = compileWith(exprA, true, err, sizeof(err));
    if (!a) {
        if (msg) snprintf(msg, (size_t)msgSize, "%s", err);
        return FILTER_UNKNOWN;
    }
    FilterProgram *b = compileWith(exprB, true, err, sizeof(err));
    if (!b) {
        if (msg) snprintf(msg, (size_t)msgSize, "%s", err);
        filterFree(a);
        return FILTER_UNKNOWN;
    }

    OverlapSearch search(a, b);
    Model model;
    const int found = search.run(&model);
    if (found > 0) {
        if (msg) describeModel(model, search, msg, msgSize);
        result = FILTER_OVERLAP;
    } else if (found == 0) {
        result = FILTER_DISJOINT;
    } else {
        if (msg) snprintf(msg, (size_t)msgSize,
                          "the two filters are too complex to compare");
        result = FILTER_UNKNOWN;
    }
    filterFree(a);
    filterFree(b);
    return result;
}
