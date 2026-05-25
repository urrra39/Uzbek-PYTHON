// =============================================================================
// uzbek.cpp — Uzbek-PY native interpreter (single-file C++17)
// =============================================================================
//
// A complete, standalone implementation of the Uzbek programming language,
// written from scratch in pure C++17 with no external dependencies.
//
// Pipeline
// --------
//   .uz source  ->  Lexer (UTF-8, INDENT/DEDENT)
//               ->  Parser (recursive descent)
//               ->  AST
//               ->  Tree-walking Interpreter
//                     - environment-chained lexical scopes
//                     - first-class functions and closures
//                     - classes with single inheritance
//                     - native Uzbek error messages
//
// Build
// -----
//   g++ -std=c++17 -O2 -Wall -Wextra -o uzbek uzbek.cpp
//   clang++ -std=c++17 -O2 -Wall -Wextra -o uzbek uzbek.cpp
//
// Run
// ---
//   ./uzbek file.uz                   - run a script
//   ./uzbek -c 'yoz("Salom!")'        - run a one-liner
//   ./uzbek                           - interactive REPL
//   ./uzbek --tokens file.uz          - dump the token stream
//   ./uzbek --ast    file.uz          - dump the AST
//
// =============================================================================


#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uzbek {

// =============================================================================
// SECTION 1 — UTF-8 utilities
// =============================================================================

namespace u8 {

// Decode the codepoint at [p, end), advance *p* past the consumed bytes.
// Returns U+FFFD on malformed input. Always advances by at least one byte
// so the lexer cannot loop forever on bad input.
inline uint32_t decode(const char*& p, const char* end) {
    if (p >= end) return 0;
    uint8_t b0 = static_cast<uint8_t>(*p++);
    if (b0 < 0x80) return b0;
    int extra;
    uint32_t cp;
    if      ((b0 & 0xE0) == 0xC0) { extra = 1; cp = b0 & 0x1F; }
    else if ((b0 & 0xF0) == 0xE0) { extra = 2; cp = b0 & 0x0F; }
    else if ((b0 & 0xF8) == 0xF0) { extra = 3; cp = b0 & 0x07; }
    else return 0xFFFD;
    while (extra--) {
        if (p >= end) return 0xFFFD;
        uint8_t b = static_cast<uint8_t>(*p++);
        if ((b & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (b & 0x3F);
    }
    return cp;
}


// Append the UTF-8 encoding of *cp* to *out*. Replaces with U+FFFD if the
// codepoint is out of the Unicode range.
inline void encode(std::string& out, uint32_t cp) {
    if (cp > 0x10FFFF) cp = 0xFFFD;
    if      (cp < 0x80)    { out.push_back(static_cast<char>(cp)); }
    else if (cp < 0x800)   { out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                             out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                             out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                             out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    else                   { out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                             out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                             out.push_back(static_cast<char>(0x80 | ((cp >>  6) & 0x3F)));
                             out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
}

// Number of codepoints in the byte range [b, e). Used by uzunlik() on strings.
inline size_t length_codepoints(const std::string& s) {
    size_t n = 0;
    const char* p = s.data();
    const char* end = p + s.size();
    while (p < end) { decode(p, end); ++n; }
    return n;
}

// Identifier character classes. The Uzbek modifier letter U+02BB (the
// glyph in `Yolgʻon`, `Boʻsh`) is admitted in the *continuation* set;
// the ASCII apostrophe is handled specially by the lexer (allowed when
// followed by another identifier character — `qo'sh`, `o'zi`).
inline bool is_ident_start(uint32_t cp) {
    if (cp == '_') return true;
    if (cp >= 'A' && cp <= 'Z') return true;
    if (cp >= 'a' && cp <= 'z') return true;
    if (cp >= 0x00C0 && cp <= 0x024F) return true;   // Latin-1 + Latin Extended-A
    if (cp >= 0x0400 && cp <= 0x04FF) return true;   // Cyrillic
    return false;
}

inline bool is_ident_cont(uint32_t cp) {
    if (is_ident_start(cp)) return true;
    if (cp >= '0' && cp <= '9') return true;
    if (cp == 0x02BB) return true;                   // ʻ MODIFIER LETTER TURNED COMMA
    return false;
}

} // namespace u8


// =============================================================================
// SECTION 2 — Source positions and errors
// =============================================================================

// 1-indexed line and column (codepoints, not bytes — friendlier for users).
// `offset` is a byte offset, used for source-region extraction.
struct Pos {
    int    line   = 1;
    int    col    = 1;
    size_t offset = 0;
};

// All compile-time and run-time failures are reported as UzbekError with a
// stable kind (used as the leading word of the rendered diagnostic) and a
// position. The `render()` helper formats the boxed multi-line message used
// by the CLI.
class UzbekError : public std::runtime_error {
public:
    std::string kind;   // e.g. "SintaksisXatoligi", "NomXatoligi"
    Pos         pos;
    std::string filename;

    UzbekError(std::string k, std::string msg, Pos p, std::string f = "<uz>")
        : std::runtime_error(std::move(msg))
        , kind(std::move(k))
        , pos(p)
        , filename(std::move(f)) {}

    std::string render(const std::string& source) const;
};

// Extract the 1-indexed *line_no* of *source* (or the empty string if out of
// range). Used by the diagnostic renderer for the source excerpt + caret.
inline std::string line_at(const std::string& source, int line_no) {
    if (line_no < 1) return {};
    int cur = 1;
    size_t start = 0;
    while (start < source.size() && cur < line_no) {
        if (source[start] == '\n') ++cur;
        ++start;
    }
    if (cur != line_no) return {};
    size_t end = start;
    while (end < source.size() && source[end] != '\n') ++end;
    return source.substr(start, end - start);
}


inline std::string UzbekError::render(const std::string& source) const {
    std::ostringstream os;
    std::string src_line = line_at(source, pos.line);
    os << "\u250F\u2501\u2501 Uzbek-PY xatolik hisoboti "
          "\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\n";
    os << "\u2503 " << kind << ": " << what() << '\n';
    os << "\u2503 Joylashuv: '" << filename << "', "
       << pos.line << "-qator, " << pos.col << "-ustun\n";
    if (!src_line.empty()) {
        os << "\u2503\n";
        os << "\u2503   " << src_line << '\n';
        if (pos.col > 0 && static_cast<size_t>(pos.col - 1) <= src_line.size()) {
            os << "\u2503   ";
            for (int i = 1; i < pos.col; ++i) os << ' ';
            os << "^\n";
        }
    }
    os << "\u2517"
          "\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501"
          "\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501"
          "\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501"
          "\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501";
    return os.str();
}


// =============================================================================
// SECTION 3 — Token types
// =============================================================================

enum class Tok {
    // Literals -----------------------------------------------------------------
    Int, Float, String, Name,

    // Keywords -----------------------------------------------------------------
    KwAgar, KwYokiAgar, KwAksHolda,
    KwUchun, KwToki, KwIchida,
    KwFunksiya, KwSinf, KwQaytarsin,
    KwRost, KwYolgon, KwBosh,
    KwVa, KwYoki, KwEmas,
    KwTanaffus, KwDavom, KwOt,        // break / continue / pass

    // Operators ----------------------------------------------------------------
    Plus, Minus, Star, Slash, DoubleSlash, Percent, StarStar,
    Eq, NotEq, Lt, Gt, Le, Ge,
    Assign,
    PlusEq, MinusEq, StarEq, SlashEq, DSlashEq, PercentEq,
    Arrow, At,

    // Punctuation --------------------------------------------------------------
    LParen, RParen, LBracket, RBracket, LBrace, RBrace,
    Comma, Colon, Semi, Dot,

    // Layout -------------------------------------------------------------------
    Newline, Indent, Dedent, Eof,
};

struct Token {
    Tok         type;
    std::string text;       // verbatim source text
    Pos         pos;
    int64_t     int_val   = 0;
    double      float_val = 0.0;
    std::string str_val;    // decoded string content (with escapes resolved)
};

// Pretty-print a token kind. Used by `--tokens` and error messages.
inline const char* tok_name(Tok t) {
    switch (t) {
        case Tok::Int: return "INT"; case Tok::Float: return "FLOAT";
        case Tok::String: return "STRING"; case Tok::Name: return "NAME";
        case Tok::KwAgar: return "agar"; case Tok::KwYokiAgar: return "yoki_agar";
        case Tok::KwAksHolda: return "aks_holda"; case Tok::KwUchun: return "uchun";
        case Tok::KwToki: return "toki"; case Tok::KwIchida: return "ichida";
        case Tok::KwFunksiya: return "funksiya"; case Tok::KwSinf: return "sinf";
        case Tok::KwQaytarsin: return "qaytarsin"; case Tok::KwRost: return "Rost";
        case Tok::KwYolgon: return "Yolgʻon"; case Tok::KwBosh: return "Boʻsh";
        case Tok::KwVa: return "va"; case Tok::KwYoki: return "yoki";
        case Tok::KwEmas: return "emas"; case Tok::KwTanaffus: return "tanaffus";
        case Tok::KwDavom: return "davom"; case Tok::KwOt: return "oʻt";
        case Tok::Plus: return "+"; case Tok::Minus: return "-";
        case Tok::Star: return "*"; case Tok::Slash: return "/";
        case Tok::DoubleSlash: return "//"; case Tok::Percent: return "%";
        case Tok::StarStar: return "**"; case Tok::Eq: return "==";
        case Tok::NotEq: return "!="; case Tok::Lt: return "<"; case Tok::Gt: return ">";
        case Tok::Le: return "<="; case Tok::Ge: return ">="; case Tok::Assign: return "=";
        case Tok::PlusEq: return "+="; case Tok::MinusEq: return "-=";
        case Tok::StarEq: return "*="; case Tok::SlashEq: return "/=";
        case Tok::DSlashEq: return "//="; case Tok::PercentEq: return "%=";
        case Tok::Arrow: return "->"; case Tok::At: return "@";
        case Tok::LParen: return "("; case Tok::RParen: return ")";
        case Tok::LBracket: return "["; case Tok::RBracket: return "]";
        case Tok::LBrace: return "{"; case Tok::RBrace: return "}";
        case Tok::Comma: return ","; case Tok::Colon: return ":";
        case Tok::Semi: return ";"; case Tok::Dot: return ".";
        case Tok::Newline: return "NEWLINE"; case Tok::Indent: return "INDENT";
        case Tok::Dedent: return "DEDENT"; case Tok::Eof: return "EOF";
    }
    return "?";
}


// =============================================================================
// SECTION 4 — Lexer
// =============================================================================

class Lexer {
public:
    Lexer(std::string source, std::string filename)
        : source_(std::move(source)), filename_(std::move(filename)) {}

    std::vector<Token> tokenize();

private:
    // --- Position helpers --------------------------------------------------
    bool   eof() const { return pos_ >= source_.size(); }
    char   peek(size_t off = 0) const {
        return pos_ + off < source_.size() ? source_[pos_ + off] : '\0';
    }
    Pos    here() const { return Pos{ line_, col_, pos_ }; }

    // Advance one *codepoint*, updating line, col, and byte offset.
    uint32_t advance_cp();

    // --- Phase helpers -----------------------------------------------------
    void   handle_line_start(std::vector<Token>& out);
    void   read_token(std::vector<Token>& out);
    Token  read_name(Pos start);
    Token  read_number(Pos start);
    Token  read_string(Pos start);
    Token  read_operator(Pos start);
    void   skip_horizontal_ws();
    void   skip_line_comment();

    // --- Indent state ------------------------------------------------------
    static int  measure_indent(const std::string& src, size_t off);

    [[noreturn]] void error(const std::string& msg, Pos p);

    std::string         source_;
    std::string         filename_;
    size_t              pos_       = 0;
    int                 line_      = 1;
    int                 col_       = 1;
    int                 paren_     = 0;   // depth of (), [], {} - controls newline handling
    bool                at_bol_    = true; // beginning of logical line?
    std::vector<int>    indents_   = {0};
};


[[noreturn]] inline void Lexer::error(const std::string& msg, Pos p) {
    throw UzbekError("LeksikXatoligi", msg, p, filename_);
}

inline uint32_t Lexer::advance_cp() {
    const char* p = source_.data() + pos_;
    const char* end = source_.data() + source_.size();
    const char* before = p;
    uint32_t cp = u8::decode(p, end);
    pos_ += static_cast<size_t>(p - before);
    if (cp == '\n') { ++line_; col_ = 1; }
    else            { ++col_; }
    return cp;
}

inline int Lexer::measure_indent(const std::string& src, size_t off) {
    int n = 0;
    while (off < src.size()) {
        char c = src[off];
        if      (c == ' ')  { ++n; ++off; }
        else if (c == '\t') { n += 8 - (n % 8); ++off; } // tabs to next 8-stop
        else                { break; }
    }
    return n;
}

inline void Lexer::skip_horizontal_ws() {
    while (!eof()) {
        char c = peek();
        if (c == ' ' || c == '\t') { ++pos_; ++col_; }
        else                       { break; }
    }
}

inline void Lexer::skip_line_comment() {
    while (!eof() && peek() != '\n') { ++pos_; ++col_; }
}


// Reserved-word table consulted after `read_name`. Keeping the keyword
// list central makes it trivial to add new keywords later.
static const std::unordered_map<std::string, Tok>& keyword_table() {
    static const std::unordered_map<std::string, Tok> M = {
        {"agar",      Tok::KwAgar},      {"yoki_agar", Tok::KwYokiAgar},
        {"aks_holda", Tok::KwAksHolda},  {"uchun",     Tok::KwUchun},
        {"toki",      Tok::KwToki},      {"ichida",    Tok::KwIchida},
        {"funksiya",  Tok::KwFunksiya},  {"sinf",      Tok::KwSinf},
        {"qaytarsin", Tok::KwQaytarsin}, {"Rost",      Tok::KwRost},
        {"Yolg\u02BBon", Tok::KwYolgon}, {"Bo\u02BBsh", Tok::KwBosh},
        {"va",        Tok::KwVa},        {"yoki",      Tok::KwYoki},
        {"emas",      Tok::KwEmas},      {"tanaffus",  Tok::KwTanaffus},
        {"davom",     Tok::KwDavom},     {"o\u02BBt",  Tok::KwOt},
    };
    return M;
}

inline std::vector<Token> Lexer::tokenize() {
    std::vector<Token> out;

    // Normalise CRLF / CR to LF up-front so the rest of the lexer can
    // assume \n is the only line separator.
    std::string norm;
    norm.reserve(source_.size());
    for (size_t i = 0; i < source_.size(); ++i) {
        if (source_[i] == '\r') {
            norm.push_back('\n');
            if (i + 1 < source_.size() && source_[i + 1] == '\n') ++i;
        } else norm.push_back(source_[i]);
    }
    source_ = std::move(norm);

    while (!eof()) {
        if (at_bol_ && paren_ == 0) {
            handle_line_start(out);
            if (eof()) break;
        }
        char c = peek();
        if (c == '\n') {
            if (paren_ == 0) {
                out.push_back(Token{Tok::Newline, "\n", here(), 0, 0.0, ""});
                at_bol_ = true;
            }
            ++pos_; ++line_; col_ = 1;
            continue;
        }
        if (c == ' ' || c == '\t') { skip_horizontal_ws(); continue; }
        if (c == '#') { skip_line_comment(); continue; }
        if (c == '\\' && peek(1) == '\n') {
            // Explicit line continuation.
            pos_ += 2; ++line_; col_ = 1;
            continue;
        }
        read_token(out);
    }

    // Synthesise NEWLINE if the file did not end with one.
    if (!out.empty() && out.back().type != Tok::Newline) {
        out.push_back(Token{Tok::Newline, "\n", here(), 0, 0.0, ""});
    }
    while (indents_.size() > 1) {
        indents_.pop_back();
        out.push_back(Token{Tok::Dedent, "", here(), 0, 0.0, ""});
    }
    out.push_back(Token{Tok::Eof, "", here(), 0, 0.0, ""});
    return out;
}


inline void Lexer::handle_line_start(std::vector<Token>& out) {
    // Skip blank-and-comment-only lines without changing the indent stack.
    while (true) {
        size_t scan = pos_;
        while (scan < source_.size() && (source_[scan] == ' ' || source_[scan] == '\t'))
            ++scan;
        if (scan == source_.size()) {
            // Trailing whitespace then EOF — bail; main loop will exit.
            return;
        }
        char c = source_[scan];
        if (c == '\n') {
            // Empty line — consume and continue at the next physical line.
            line_ += 1;
            col_ = 1;
            pos_ = scan + 1;
            continue;
        }
        if (c == '#') {
            // Comment-only line — consume the comment + newline.
            pos_ = scan;
            col_ = 1 + static_cast<int>(scan - (pos_ - col_ + 1));
            // Recompute col_ correctly:
            // It is easier to just walk: we already know the indent was
            // pure ASCII whitespace, so column == scan - line_start + 1.
            // Re-derive line_start by stepping backwards on the original.
            // For simplicity: just skip the comment then the newline.
            while (pos_ < source_.size() && source_[pos_] != '\n') ++pos_;
            if (pos_ < source_.size()) { ++pos_; ++line_; col_ = 1; }
            continue;
        }
        // Real content on this line — apply INDENT/DEDENT logic.
        int indent = measure_indent(source_, pos_);
        // Advance pos_/col_ past the leading whitespace.
        while (pos_ < source_.size() && (source_[pos_] == ' ' || source_[pos_] == '\t')) {
            ++pos_; ++col_;
        }
        int top = indents_.back();
        if (indent > top) {
            indents_.push_back(indent);
            out.push_back(Token{Tok::Indent, "", here(), 0, 0.0, ""});
        } else if (indent < top) {
            while (indents_.size() > 1 && indents_.back() > indent) {
                indents_.pop_back();
                out.push_back(Token{Tok::Dedent, "", here(), 0, 0.0, ""});
            }
            if (indents_.back() != indent) {
                error("chekinish darajalari mos kelmaydi", here());
            }
        }
        at_bol_ = false;
        return;
    }
}


inline void Lexer::read_token(std::vector<Token>& out) {
    Pos start = here();
    char c = peek();

    // String literal
    if (c == '"' || c == '\'') {
        out.push_back(read_string(start));
        return;
    }
    // Number — leading digit, or '.' followed by digit
    if ((c >= '0' && c <= '9') ||
        (c == '.' && peek(1) >= '0' && peek(1) <= '9')) {
        out.push_back(read_number(start));
        return;
    }
    // Identifier / keyword: peek the first codepoint
    {
        const char* p = source_.data() + pos_;
        const char* end = source_.data() + source_.size();
        const char* save = p;
        uint32_t cp = u8::decode(p, end);
        if (u8::is_ident_start(cp)) {
            (void)save;
            out.push_back(read_name(start));
            return;
        }
    }
    out.push_back(read_operator(start));
}


inline Token Lexer::read_name(Pos start) {
    const char* base = source_.data();
    size_t begin = pos_;
    // First codepoint is known to satisfy is_ident_start.
    advance_cp();
    while (!eof()) {
        const char* p = base + pos_;
        const char* end = base + source_.size();
        const char* save = p;
        uint32_t cp = u8::decode(p, end);
        if (u8::is_ident_cont(cp)) {
            pos_ += static_cast<size_t>(p - save); ++col_;
            continue;
        }
        // Mid-identifier ASCII apostrophe — only when followed by another
        // identifier-start character. This is what lets `qo'sh`, `o'zi`,
        // `xavfsiz_bo'lish` survive as single tokens.
        if (cp == '\'' ) {
            const char* q = p; // already past the apostrophe
            uint32_t next_cp = u8::decode(q, end);
            if (u8::is_ident_start(next_cp)) {
                pos_ += static_cast<size_t>(p - save); ++col_;
                continue;
            }
        }
        break;
    }
    std::string text(source_.data() + begin, pos_ - begin);
    auto it = keyword_table().find(text);
    Token t;
    t.type = it != keyword_table().end() ? it->second : Tok::Name;
    t.text = text;
    t.pos  = start;
    return t;
}


inline Token Lexer::read_number(Pos start) {
    size_t begin = pos_;
    bool is_float = false;

    // 0x / 0o / 0b prefixes (integer only).
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X' ||
                          peek(1) == 'o' || peek(1) == 'O' ||
                          peek(1) == 'b' || peek(1) == 'B')) {
        pos_ += 2; col_ += 2;
        while (!eof()) {
            char c = peek();
            bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                      (c >= 'A' && c <= 'F') || c == '_';
            if (!ok) break;
            ++pos_; ++col_;
        }
    } else {
        while (!eof()) {
            char c = peek();
            if ((c >= '0' && c <= '9') || c == '_') { ++pos_; ++col_; }
            else break;
        }
        if (peek() == '.' && peek(1) >= '0' && peek(1) <= '9') {
            is_float = true;
            ++pos_; ++col_;
            while (!eof()) {
                char c = peek();
                if ((c >= '0' && c <= '9') || c == '_') { ++pos_; ++col_; }
                else break;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            is_float = true;
            ++pos_; ++col_;
            if (peek() == '+' || peek() == '-') { ++pos_; ++col_; }
            while (!eof()) {
                char c = peek();
                if ((c >= '0' && c <= '9') || c == '_') { ++pos_; ++col_; }
                else break;
            }
        }
    }

    std::string text(source_.data() + begin, pos_ - begin);
    std::string clean;
    clean.reserve(text.size());
    for (char ch : text) if (ch != '_') clean.push_back(ch);

    Token t;
    t.text = text;
    t.pos  = start;
    if (is_float) {
        t.type = Tok::Float;
        t.float_val = std::stod(clean);
    } else {
        t.type = Tok::Int;
        try {
            if (clean.size() > 2 && clean[0] == '0' && (clean[1] == 'x' || clean[1] == 'X'))
                t.int_val = std::stoll(clean.substr(2), nullptr, 16);
            else if (clean.size() > 2 && clean[0] == '0' && (clean[1] == 'o' || clean[1] == 'O'))
                t.int_val = std::stoll(clean.substr(2), nullptr, 8);
            else if (clean.size() > 2 && clean[0] == '0' && (clean[1] == 'b' || clean[1] == 'B'))
                t.int_val = std::stoll(clean.substr(2), nullptr, 2);
            else
                t.int_val = std::stoll(clean);
        } catch (...) {
            error("son literali noto'g'ri yoki juda katta", start);
        }
    }
    return t;
}


inline Token Lexer::read_string(Pos start) {
    char quote = peek();
    bool triple = (peek(1) == quote && peek(2) == quote);
    size_t prefix = triple ? 3 : 1;
    pos_ += prefix; col_ += static_cast<int>(prefix);

    std::string body;
    while (!eof()) {
        if (triple) {
            if (peek() == quote && peek(1) == quote && peek(2) == quote) {
                pos_ += 3; col_ += 3;
                Token t;
                t.type = Tok::String; t.text = body; t.pos = start; t.str_val = body;
                return t;
            }
        } else if (peek() == quote) {
            ++pos_; ++col_;
            Token t;
            t.type = Tok::String; t.text = body; t.pos = start; t.str_val = body;
            return t;
        }
        char c = peek();
        if (c == '\n' && !triple) {
            error("bir qatorli satr literali yopilmagan", start);
        }
        if (c == '\\') {
            ++pos_; ++col_;
            if (eof()) error("satr literali yopilmagan (qochish belgisi)", start);
            char esc = peek(); ++pos_; ++col_;
            switch (esc) {
                case 'n':  body.push_back('\n'); break;
                case 't':  body.push_back('\t'); break;
                case 'r':  body.push_back('\r'); break;
                case '0':  body.push_back('\0'); break;
                case '\\': body.push_back('\\'); break;
                case '\'': body.push_back('\''); break;
                case '"':  body.push_back('"');  break;
                case '\n': /* line continuation inside string */ ++line_; col_ = 1; break;
                case 'x': {
                    if (pos_ + 2 > source_.size())
                        error("\\xNN qochish belgisi tugamagan", here());
                    std::string hex = source_.substr(pos_, 2);
                    pos_ += 2; col_ += 2;
                    body.push_back(static_cast<char>(std::stoi(hex, nullptr, 16)));
                    break;
                }
                case 'u': {
                    if (pos_ + 4 > source_.size())
                        error("\\uNNNN qochish belgisi tugamagan", here());
                    std::string hex = source_.substr(pos_, 4);
                    pos_ += 4; col_ += 4;
                    u8::encode(body, static_cast<uint32_t>(std::stoi(hex, nullptr, 16)));
                    break;
                }
                default:
                    body.push_back('\\'); body.push_back(esc);
            }
            continue;
        }
        // Verbatim character — could be multi-byte UTF-8.
        if (c == '\n') { ++line_; col_ = 1; ++pos_; body.push_back('\n'); }
        else           { ++pos_; ++col_; body.push_back(c); }
    }
    error("satr literali yopilmagan (fayl oxiri)", start);
}


inline Token Lexer::read_operator(Pos start) {
    char c0 = peek();
    char c1 = peek(1);
    char c2 = peek(2);
    auto make = [&](Tok t, int len) {
        Token tok;
        tok.type = t;
        tok.text = source_.substr(pos_, static_cast<size_t>(len));
        tok.pos  = start;
        pos_ += static_cast<size_t>(len); col_ += len;
        return tok;
    };

    // Three-char ops
    if (c0 == '/' && c1 == '/' && c2 == '=') return make(Tok::DSlashEq, 3);
    if (c0 == '*' && c1 == '*' && c2 == '=') return make(Tok::StarStar, 3);  // (no **= for v1)

    // Two-char ops
    if (c0 == '/' && c1 == '/') return make(Tok::DoubleSlash, 2);
    if (c0 == '*' && c1 == '*') return make(Tok::StarStar, 2);
    if (c0 == '=' && c1 == '=') return make(Tok::Eq, 2);
    if (c0 == '!' && c1 == '=') return make(Tok::NotEq, 2);
    if (c0 == '<' && c1 == '=') return make(Tok::Le, 2);
    if (c0 == '>' && c1 == '=') return make(Tok::Ge, 2);
    if (c0 == '+' && c1 == '=') return make(Tok::PlusEq, 2);
    if (c0 == '-' && c1 == '=') return make(Tok::MinusEq, 2);
    if (c0 == '*' && c1 == '=') return make(Tok::StarEq, 2);
    if (c0 == '/' && c1 == '=') return make(Tok::SlashEq, 2);
    if (c0 == '%' && c1 == '=') return make(Tok::PercentEq, 2);
    if (c0 == '-' && c1 == '>') return make(Tok::Arrow, 2);

    // Single-char ops / punctuation
    switch (c0) {
        case '+': return make(Tok::Plus, 1);
        case '-': return make(Tok::Minus, 1);
        case '*': return make(Tok::Star, 1);
        case '/': return make(Tok::Slash, 1);
        case '%': return make(Tok::Percent, 1);
        case '@': return make(Tok::At, 1);
        case '<': return make(Tok::Lt, 1);
        case '>': return make(Tok::Gt, 1);
        case '=': return make(Tok::Assign, 1);
        case '(': ++paren_; return make(Tok::LParen, 1);
        case ')': if (paren_ > 0) --paren_; return make(Tok::RParen, 1);
        case '[': ++paren_; return make(Tok::LBracket, 1);
        case ']': if (paren_ > 0) --paren_; return make(Tok::RBracket, 1);
        case '{': ++paren_; return make(Tok::LBrace, 1);
        case '}': if (paren_ > 0) --paren_; return make(Tok::RBrace, 1);
        case ',': return make(Tok::Comma, 1);
        case ':': return make(Tok::Colon, 1);
        case ';': return make(Tok::Semi, 1);
        case '.': return make(Tok::Dot, 1);
    }
    error(std::string("kutilmagan belgi: '") + c0 + "'", start);
}


// =============================================================================
// SECTION 5 — AST
// =============================================================================
//
// Each AST node carries an enum *kind* tag so the interpreter can dispatch via
// a single `switch` (faster than virtual dispatch and easier to inline). Nodes
// are owned by `std::unique_ptr` while the tree is alive; the interpreter only
// borrows raw pointers, never extends ownership.

class Expr; class Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

class Expr {
public:
    enum class Kind {
        Int, Float, Str, Bool, None, Name,
        Binary, Unary, Logical, Compare, Membership,
        Call, Index, Attr, List, Dict,
    };
    Kind kind;
    Pos  pos;
    explicit Expr(Kind k) : kind(k) {}
    virtual ~Expr() = default;
};

class Stmt {
public:
    enum class Kind {
        ExprS, Assign, AugAssign,
        If, While, For,
        FuncDef, ClassDef,
        Return, Break, Continue, Pass,
    };
    Kind kind;
    Pos  pos;
    explicit Stmt(Kind k) : kind(k) {}
    virtual ~Stmt() = default;
};


// --- Expression nodes --------------------------------------------------------

struct IntLit    : Expr { int64_t value; IntLit()    : Expr(Kind::Int)   {} };
struct FloatLit  : Expr { double  value; FloatLit()  : Expr(Kind::Float) {} };
struct StrLit    : Expr { std::string value; StrLit() : Expr(Kind::Str) {} };
struct BoolLit   : Expr { bool value;    BoolLit()   : Expr(Kind::Bool)  {} };
struct NoneLit   : Expr {                NoneLit()   : Expr(Kind::None)  {} };
struct NameExpr  : Expr { std::string name; NameExpr() : Expr(Kind::Name){} };

struct BinaryExpr : Expr {
    Tok op; ExprPtr left, right;
    BinaryExpr() : Expr(Kind::Binary) {}
};
struct UnaryExpr : Expr {
    Tok op; ExprPtr operand;
    UnaryExpr() : Expr(Kind::Unary) {}
};
struct LogicalExpr : Expr {
    Tok op; ExprPtr left, right;
    LogicalExpr() : Expr(Kind::Logical) {}
};
struct CompareExpr : Expr {
    Tok op; ExprPtr left, right;
    CompareExpr() : Expr(Kind::Compare) {}
};
struct MembershipExpr : Expr {
    bool negate = false;     // emas ichida ...
    ExprPtr left, container;
    MembershipExpr() : Expr(Kind::Membership) {}
};
struct CallExpr : Expr {
    ExprPtr callee;
    std::vector<ExprPtr> args;
    CallExpr() : Expr(Kind::Call) {}
};
struct IndexExpr : Expr {
    ExprPtr target, index;
    IndexExpr() : Expr(Kind::Index) {}
};
struct AttrExpr : Expr {
    ExprPtr target;
    std::string name;
    AttrExpr() : Expr(Kind::Attr) {}
};
struct ListExpr : Expr {
    std::vector<ExprPtr> elements;
    ListExpr() : Expr(Kind::List) {}
};
struct DictExpr : Expr {
    std::vector<std::pair<ExprPtr, ExprPtr>> items;
    DictExpr() : Expr(Kind::Dict) {}
};


// --- Statement nodes ---------------------------------------------------------

struct ExprStmt : Stmt {
    ExprPtr expr;
    ExprStmt() : Stmt(Kind::ExprS) {}
};
struct AssignStmt : Stmt {
    // target is one of NameExpr / IndexExpr / AttrExpr — validated by the
    // parser before construction.
    ExprPtr target;
    ExprPtr value;
    AssignStmt() : Stmt(Kind::Assign) {}
};
struct AugAssignStmt : Stmt {
    Tok op;             // PlusEq, MinusEq, ...
    ExprPtr target;
    ExprPtr value;
    AugAssignStmt() : Stmt(Kind::AugAssign) {}
};
struct IfStmt : Stmt {
    ExprPtr cond;
    std::vector<StmtPtr> then_body;
    std::vector<std::pair<ExprPtr, std::vector<StmtPtr>>> elifs;
    std::vector<StmtPtr> else_body;     // empty => no else clause
    bool has_else = false;
    IfStmt() : Stmt(Kind::If) {}
};
struct WhileStmt : Stmt {
    ExprPtr cond;
    std::vector<StmtPtr> body;
    WhileStmt() : Stmt(Kind::While) {}
};
struct ForStmt : Stmt {
    std::string var;
    ExprPtr iter;
    std::vector<StmtPtr> body;
    ForStmt() : Stmt(Kind::For) {}
};
struct FuncDefStmt : Stmt {
    std::string name;
    std::vector<std::string> params;
    std::vector<StmtPtr> body;
    FuncDefStmt() : Stmt(Kind::FuncDef) {}
};
struct ClassDefStmt : Stmt {
    std::string name;
    std::string base;       // empty if no base
    std::vector<std::unique_ptr<FuncDefStmt>> methods;
    ClassDefStmt() : Stmt(Kind::ClassDef) {}
};
struct ReturnStmt : Stmt {
    ExprPtr value;          // nullptr means "no expression"
    ReturnStmt() : Stmt(Kind::Return) {}
};
struct BreakStmt    : Stmt { BreakStmt()    : Stmt(Kind::Break)    {} };
struct ContinueStmt : Stmt { ContinueStmt() : Stmt(Kind::Continue) {} };
struct PassStmt     : Stmt { PassStmt()     : Stmt(Kind::Pass)     {} };


// =============================================================================
// SECTION 6 — Parser
// =============================================================================

class Parser {
public:
    Parser(std::vector<Token> tokens, std::string filename)
        : toks_(std::move(tokens)), filename_(std::move(filename)) {}

    std::vector<StmtPtr> parse_program();

private:
    // --- Token cursor ---
    const Token& peek(size_t off = 0) const { return toks_[std::min(idx_ + off, toks_.size() - 1)]; }
    bool check(Tok t) const { return peek().type == t; }
    bool match(Tok t) { if (check(t)) { ++idx_; return true; } return false; }
    const Token& consume() { return toks_[idx_++]; }
    const Token& expect(Tok t, const char* what);
    void skip_newlines() { while (check(Tok::Newline)) ++idx_; }
    [[noreturn]] void error(const std::string& msg, Pos p) {
        throw UzbekError("SintaksisXatoligi", msg, p, filename_);
    }

    // --- Statements ---
    StmtPtr parse_statement();
    StmtPtr parse_simple_stmt();
    StmtPtr parse_compound_stmt();
    StmtPtr parse_if();
    StmtPtr parse_while();
    StmtPtr parse_for();
    StmtPtr parse_funcdef();
    StmtPtr parse_classdef();
    std::vector<StmtPtr> parse_suite();

    // --- Expressions (precedence cascade) ---
    ExprPtr parse_expr();
    ExprPtr parse_or();
    ExprPtr parse_and();
    ExprPtr parse_not();
    ExprPtr parse_comparison();
    ExprPtr parse_arith();
    ExprPtr parse_term();
    ExprPtr parse_factor();
    ExprPtr parse_power();
    ExprPtr parse_postfix();
    ExprPtr parse_atom();
    std::vector<ExprPtr> parse_call_args();

    std::vector<Token> toks_;
    std::string        filename_;
    size_t             idx_ = 0;
};

inline const Token& Parser::expect(Tok t, const char* what) {
    if (peek().type != t) {
        std::ostringstream os;
        os << what << " kutilgan edi, lekin '" << tok_name(peek().type) << "' topildi";
        error(os.str(), peek().pos);
    }
    return consume();
}


inline std::vector<StmtPtr> Parser::parse_program() {
    std::vector<StmtPtr> out;
    skip_newlines();
    while (!check(Tok::Eof)) {
        out.push_back(parse_statement());
        skip_newlines();
    }
    return out;
}

inline StmtPtr Parser::parse_statement() {
    switch (peek().type) {
        case Tok::KwAgar:     return parse_if();
        case Tok::KwToki:     return parse_while();
        case Tok::KwUchun:    return parse_for();
        case Tok::KwFunksiya: return parse_funcdef();
        case Tok::KwSinf:     return parse_classdef();
        default:              return parse_simple_stmt();
    }
}

inline StmtPtr Parser::parse_simple_stmt() {
    Pos p = peek().pos;
    switch (peek().type) {
        case Tok::KwQaytarsin: {
            ++idx_;
            auto s = std::make_unique<ReturnStmt>();
            s->pos = p;
            if (!check(Tok::Newline) && !check(Tok::Semi) && !check(Tok::Eof))
                s->value = parse_expr();
            if (!match(Tok::Semi)) expect(Tok::Newline, "qator oxiri");
            return s;
        }
        case Tok::KwTanaffus: {
            ++idx_;
            auto s = std::make_unique<BreakStmt>();
            s->pos = p;
            if (!match(Tok::Semi)) expect(Tok::Newline, "qator oxiri");
            return s;
        }
        case Tok::KwDavom: {
            ++idx_;
            auto s = std::make_unique<ContinueStmt>();
            s->pos = p;
            if (!match(Tok::Semi)) expect(Tok::Newline, "qator oxiri");
            return s;
        }
        case Tok::KwOt: {
            ++idx_;
            auto s = std::make_unique<PassStmt>();
            s->pos = p;
            if (!match(Tok::Semi)) expect(Tok::Newline, "qator oxiri");
            return s;
        }
        default: break;
    }

    // Expression / assignment / augmented assignment.
    ExprPtr lhs = parse_expr();

    auto is_aug = [&](Tok t) {
        return t == Tok::PlusEq || t == Tok::MinusEq || t == Tok::StarEq ||
               t == Tok::SlashEq || t == Tok::DSlashEq || t == Tok::PercentEq;
    };

    if (check(Tok::Assign)) {
        ++idx_;
        ExprPtr rhs = parse_expr();
        if (lhs->kind != Expr::Kind::Name &&
            lhs->kind != Expr::Kind::Index &&
            lhs->kind != Expr::Kind::Attr)
            error("o'zlashtirish chap tomoni nomi, indeksi yoki atributi bo'lishi shart", lhs->pos);
        auto s = std::make_unique<AssignStmt>();
        s->pos = p; s->target = std::move(lhs); s->value = std::move(rhs);
        if (!match(Tok::Semi)) expect(Tok::Newline, "qator oxiri");
        return s;
    }
    if (is_aug(peek().type)) {
        Tok op = consume().type;
        ExprPtr rhs = parse_expr();
        if (lhs->kind != Expr::Kind::Name &&
            lhs->kind != Expr::Kind::Index &&
            lhs->kind != Expr::Kind::Attr)
            error("kengaytirilgan o'zlashtirish chap tomoni nomi yoki indeks bo'lishi shart", lhs->pos);
        auto s = std::make_unique<AugAssignStmt>();
        s->pos = p; s->op = op; s->target = std::move(lhs); s->value = std::move(rhs);
        if (!match(Tok::Semi)) expect(Tok::Newline, "qator oxiri");
        return s;
    }

    auto s = std::make_unique<ExprStmt>();
    s->pos = p; s->expr = std::move(lhs);
    if (!match(Tok::Semi)) expect(Tok::Newline, "qator oxiri");
    return s;
}


inline std::vector<StmtPtr> Parser::parse_suite() {
    // suite -> NEWLINE INDENT statement+ DEDENT  |  simple_stmt
    if (match(Tok::Newline)) {
        skip_newlines();
        if (!check(Tok::Indent))
            error("chekingan blok kutilgan edi", peek().pos);
        ++idx_;  // INDENT
        std::vector<StmtPtr> body;
        while (!check(Tok::Dedent) && !check(Tok::Eof)) {
            body.push_back(parse_statement());
            skip_newlines();
        }
        if (check(Tok::Dedent)) ++idx_;
        if (body.empty())
            error("blokda kamida bitta ifoda bo'lishi shart", peek().pos);
        return body;
    }
    // Single inline statement after the colon.
    std::vector<StmtPtr> body;
    body.push_back(parse_simple_stmt());
    return body;
}

inline StmtPtr Parser::parse_if() {
    Pos p = peek().pos;
    ++idx_;  // 'agar'
    auto s = std::make_unique<IfStmt>();
    s->pos = p;
    s->cond = parse_expr();
    expect(Tok::Colon, "':'");
    s->then_body = parse_suite();
    while (check(Tok::KwYokiAgar)) {
        ++idx_;
        ExprPtr c = parse_expr();
        expect(Tok::Colon, "':'");
        std::vector<StmtPtr> body = parse_suite();
        s->elifs.emplace_back(std::move(c), std::move(body));
    }
    if (check(Tok::KwAksHolda)) {
        ++idx_;
        expect(Tok::Colon, "':'");
        s->else_body = parse_suite();
        s->has_else  = true;
    }
    return s;
}

inline StmtPtr Parser::parse_while() {
    Pos p = peek().pos;
    ++idx_;
    auto s = std::make_unique<WhileStmt>();
    s->pos = p;
    s->cond = parse_expr();
    expect(Tok::Colon, "':'");
    s->body = parse_suite();
    return s;
}

inline StmtPtr Parser::parse_for() {
    Pos p = peek().pos;
    ++idx_;
    auto s = std::make_unique<ForStmt>();
    s->pos = p;
    const Token& nm = expect(Tok::Name, "tsikl o'zgaruvchi nomi");
    s->var = nm.text;
    expect(Tok::KwIchida, "'ichida'");
    s->iter = parse_expr();
    expect(Tok::Colon, "':'");
    s->body = parse_suite();
    return s;
}


inline StmtPtr Parser::parse_funcdef() {
    Pos p = peek().pos;
    ++idx_;  // 'funksiya'
    auto s = std::make_unique<FuncDefStmt>();
    s->pos = p;
    const Token& nm = expect(Tok::Name, "funksiya nomi");
    s->name = nm.text;
    expect(Tok::LParen, "'('");
    if (!check(Tok::RParen)) {
        do {
            const Token& pn = expect(Tok::Name, "parametr nomi");
            s->params.push_back(pn.text);
        } while (match(Tok::Comma));
    }
    expect(Tok::RParen, "')'");
    expect(Tok::Colon, "':'");
    s->body = parse_suite();
    return s;
}

inline StmtPtr Parser::parse_classdef() {
    Pos p = peek().pos;
    ++idx_;
    auto s = std::make_unique<ClassDefStmt>();
    s->pos = p;
    const Token& nm = expect(Tok::Name, "sinf nomi");
    s->name = nm.text;
    if (match(Tok::LParen)) {
        if (!check(Tok::RParen)) {
            const Token& base = expect(Tok::Name, "ota-sinf nomi");
            s->base = base.text;
        }
        expect(Tok::RParen, "')'");
    }
    expect(Tok::Colon, "':'");

    // Class body — sequence of methods (and pass-stmt placeholders).
    if (match(Tok::Newline)) {
        skip_newlines();
        if (!check(Tok::Indent))
            error("sinf tanasi chekingan bo'lishi shart", peek().pos);
        ++idx_;
        while (!check(Tok::Dedent) && !check(Tok::Eof)) {
            if (check(Tok::KwOt)) {
                ++idx_;
                if (!match(Tok::Semi)) expect(Tok::Newline, "qator oxiri");
                skip_newlines();
                continue;
            }
            if (check(Tok::KwFunksiya)) {
                StmtPtr st = parse_funcdef();
                auto* raw = static_cast<FuncDefStmt*>(st.release());
                s->methods.emplace_back(std::unique_ptr<FuncDefStmt>(raw));
            } else {
                error("sinf tanasi ichida faqat 'funksiya' yoki 'o\u02BBt' bo'lishi mumkin", peek().pos);
            }
            skip_newlines();
        }
        if (check(Tok::Dedent)) ++idx_;
    } else {
        // Inline 'pass' on the same line — `sinf X: o't`.
        if (!check(Tok::KwOt))
            error("sinf tanasi yoki 'o\u02BBt' kutilgan edi", peek().pos);
        ++idx_;
        if (!match(Tok::Semi)) expect(Tok::Newline, "qator oxiri");
    }
    return s;
}


// --- Expressions: precedence cascade -----------------------------------------

inline ExprPtr Parser::parse_expr() { return parse_or(); }

inline ExprPtr Parser::parse_or() {
    ExprPtr left = parse_and();
    while (check(Tok::KwYoki)) {
        Pos p = peek().pos; ++idx_;
        ExprPtr right = parse_and();
        auto n = std::make_unique<LogicalExpr>();
        n->pos = p; n->op = Tok::KwYoki;
        n->left = std::move(left); n->right = std::move(right);
        left = std::move(n);
    }
    return left;
}

inline ExprPtr Parser::parse_and() {
    ExprPtr left = parse_not();
    while (check(Tok::KwVa)) {
        Pos p = peek().pos; ++idx_;
        ExprPtr right = parse_not();
        auto n = std::make_unique<LogicalExpr>();
        n->pos = p; n->op = Tok::KwVa;
        n->left = std::move(left); n->right = std::move(right);
        left = std::move(n);
    }
    return left;
}

inline ExprPtr Parser::parse_not() {
    if (check(Tok::KwEmas)) {
        Pos p = peek().pos; ++idx_;
        // Lookahead: `emas ichida` is a membership variant, not a unary.
        if (check(Tok::KwIchida)) {
            error("'emas' dan keyin ifoda kutilgan edi", peek().pos);
        }
        auto n = std::make_unique<UnaryExpr>();
        n->pos = p; n->op = Tok::KwEmas;
        n->operand = parse_not();
        return n;
    }
    return parse_comparison();
}


inline ExprPtr Parser::parse_comparison() {
    ExprPtr left = parse_arith();
    // Single binary comparison or a membership test.
    if (check(Tok::Eq) || check(Tok::NotEq) ||
        check(Tok::Lt) || check(Tok::Gt) ||
        check(Tok::Le) || check(Tok::Ge)) {
        Tok op = consume().type;
        ExprPtr right = parse_arith();
        auto n = std::make_unique<CompareExpr>();
        n->pos = left->pos; n->op = op;
        n->left = std::move(left); n->right = std::move(right);
        return n;
    }
    if (check(Tok::KwIchida)) {
        Pos p = peek().pos; ++idx_;
        ExprPtr cont = parse_arith();
        auto n = std::make_unique<MembershipExpr>();
        n->pos = p; n->left = std::move(left);
        n->container = std::move(cont);
        n->negate = false;
        return n;
    }
    return left;
}

inline ExprPtr Parser::parse_arith() {
    ExprPtr left = parse_term();
    while (check(Tok::Plus) || check(Tok::Minus)) {
        Tok op = consume().type;
        ExprPtr right = parse_term();
        auto n = std::make_unique<BinaryExpr>();
        n->pos = left->pos; n->op = op;
        n->left = std::move(left); n->right = std::move(right);
        left = std::move(n);
    }
    return left;
}

inline ExprPtr Parser::parse_term() {
    ExprPtr left = parse_factor();
    while (check(Tok::Star) || check(Tok::Slash) ||
           check(Tok::DoubleSlash) || check(Tok::Percent)) {
        Tok op = consume().type;
        ExprPtr right = parse_factor();
        auto n = std::make_unique<BinaryExpr>();
        n->pos = left->pos; n->op = op;
        n->left = std::move(left); n->right = std::move(right);
        left = std::move(n);
    }
    return left;
}

inline ExprPtr Parser::parse_factor() {
    if (check(Tok::Plus) || check(Tok::Minus)) {
        Tok op = consume().type;
        auto n = std::make_unique<UnaryExpr>();
        n->pos = peek().pos; n->op = op;
        n->operand = parse_factor();
        return n;
    }
    return parse_power();
}

inline ExprPtr Parser::parse_power() {
    ExprPtr base = parse_postfix();
    if (check(Tok::StarStar)) {
        ++idx_;
        ExprPtr exp = parse_factor();   // right-associative
        auto n = std::make_unique<BinaryExpr>();
        n->pos = base->pos; n->op = Tok::StarStar;
        n->left = std::move(base); n->right = std::move(exp);
        return n;
    }
    return base;
}


inline ExprPtr Parser::parse_postfix() {
    ExprPtr e = parse_atom();
    while (true) {
        if (check(Tok::LParen)) {
            ++idx_;
            std::vector<ExprPtr> args = parse_call_args();
            expect(Tok::RParen, "')'");
            auto n = std::make_unique<CallExpr>();
            n->pos = e->pos; n->callee = std::move(e);
            n->args = std::move(args);
            e = std::move(n);
        } else if (check(Tok::LBracket)) {
            ++idx_;
            ExprPtr idx = parse_expr();
            expect(Tok::RBracket, "']'");
            auto n = std::make_unique<IndexExpr>();
            n->pos = e->pos; n->target = std::move(e);
            n->index = std::move(idx);
            e = std::move(n);
        } else if (check(Tok::Dot)) {
            ++idx_;
            const Token& nm = expect(Tok::Name, "atribut nomi");
            auto n = std::make_unique<AttrExpr>();
            n->pos = e->pos; n->target = std::move(e);
            n->name = nm.text;
            e = std::move(n);
        } else {
            break;
        }
    }
    return e;
}

inline std::vector<ExprPtr> Parser::parse_call_args() {
    std::vector<ExprPtr> out;
    if (check(Tok::RParen)) return out;
    out.push_back(parse_expr());
    while (match(Tok::Comma)) {
        if (check(Tok::RParen)) break;     // trailing comma allowed
        out.push_back(parse_expr());
    }
    return out;
}


inline ExprPtr Parser::parse_atom() {
    Pos p = peek().pos;
    switch (peek().type) {
        case Tok::Int: {
            auto n = std::make_unique<IntLit>();
            n->pos = p; n->value = consume().int_val;
            return n;
        }
        case Tok::Float: {
            auto n = std::make_unique<FloatLit>();
            n->pos = p; n->value = consume().float_val;
            return n;
        }
        case Tok::String: {
            auto n = std::make_unique<StrLit>();
            n->pos = p; n->value = consume().str_val;
            // Concatenate adjacent string literals (Python-style).
            while (check(Tok::String)) n->value += consume().str_val;
            return n;
        }
        case Tok::KwRost: {
            ++idx_;
            auto n = std::make_unique<BoolLit>();
            n->pos = p; n->value = true; return n;
        }
        case Tok::KwYolgon: {
            ++idx_;
            auto n = std::make_unique<BoolLit>();
            n->pos = p; n->value = false; return n;
        }
        case Tok::KwBosh: {
            ++idx_;
            auto n = std::make_unique<NoneLit>();
            n->pos = p; return n;
        }
        case Tok::Name: {
            auto n = std::make_unique<NameExpr>();
            n->pos = p; n->name = consume().text;
            return n;
        }
        case Tok::LParen: {
            ++idx_;
            ExprPtr e = parse_expr();
            expect(Tok::RParen, "')'");
            return e;
        }
        case Tok::LBracket: {
            ++idx_;
            auto n = std::make_unique<ListExpr>();
            n->pos = p;
            if (!check(Tok::RBracket)) {
                n->elements.push_back(parse_expr());
                while (match(Tok::Comma)) {
                    if (check(Tok::RBracket)) break;
                    n->elements.push_back(parse_expr());
                }
            }
            expect(Tok::RBracket, "']'");
            return n;
        }
        case Tok::LBrace: {
            ++idx_;
            auto n = std::make_unique<DictExpr>();
            n->pos = p;
            if (!check(Tok::RBrace)) {
                ExprPtr k = parse_expr();
                expect(Tok::Colon, "':'");
                ExprPtr v = parse_expr();
                n->items.emplace_back(std::move(k), std::move(v));
                while (match(Tok::Comma)) {
                    if (check(Tok::RBrace)) break;
                    ExprPtr k2 = parse_expr();
                    expect(Tok::Colon, "':'");
                    ExprPtr v2 = parse_expr();
                    n->items.emplace_back(std::move(k2), std::move(v2));
                }
            }
            expect(Tok::RBrace, "'}'");
            return n;
        }
        default: break;
    }
    error(std::string("ifoda kutilgan edi, lekin '") + tok_name(peek().type) + "' topildi", p);
}


// =============================================================================
// SECTION 7 — Values and heap objects
// =============================================================================
//
// The runtime uses a single `Value` type that carries a discriminating tag
// plus the union of every concrete representation. Heap-backed objects (list,
// dict, function, class, instance, bound method) live behind `std::shared_ptr`
// so they share value semantics with safe automatic cleanup.
//
// Header dance — the chicken-and-egg between Value and its heap object types
// (which carry Value-typed members) is resolved by:
//   1. forward-declaring everything,
//   2. defining `Value` first (uses only `shared_ptr<X>` of forward types),
//   3. defining the structs after Value is complete.

class Env;
class Value;
struct ListObj;
struct DictObj;
struct FunctionObj;
struct ClassObj;
struct InstanceObj;
struct BoundMethodObj;

// Built-in C++ function pointer signature. The arg vector is mutable so
// builtins can move out values. The Pos is the call site (for diagnostics).
using BuiltinFn = Value(*)(std::vector<Value>&, Pos);


class Value {
public:
    enum class Tag {
        Nil, Bool, Int, Float, Str, List, Dict,
        Func, Builtin, Class, Instance, BoundMethod,
    };
    Tag tag = Tag::Nil;

    // Inline primitives.
    bool    b = false;
    int64_t i = 0;
    double  d = 0.0;

    // Heap-backed objects. Only the field corresponding to `tag` is populated;
    // the others remain default-constructed (one null pointer each).
    std::shared_ptr<std::string>     s;
    std::shared_ptr<ListObj>         list;
    std::shared_ptr<DictObj>         dict;
    std::shared_ptr<FunctionObj>     func;
    std::shared_ptr<ClassObj>        cls;
    std::shared_ptr<InstanceObj>     inst;
    std::shared_ptr<BoundMethodObj>  meth;
    BuiltinFn                        builtin = nullptr;

    // ---- factory helpers -------------------------------------------------
    static Value Nil()                  { return {}; }
    static Value B(bool v)              { Value x; x.tag = Tag::Bool;  x.b = v; return x; }
    static Value I(int64_t v)           { Value x; x.tag = Tag::Int;   x.i = v; return x; }
    static Value F(double v)            { Value x; x.tag = Tag::Float; x.d = v; return x; }
    static Value Str(std::string v)     { Value x; x.tag = Tag::Str;
                                          x.s = std::make_shared<std::string>(std::move(v));
                                          return x; }
    static Value List_()                { Value x; x.tag = Tag::List;
                                          x.list = std::make_shared<ListObj>();
                                          return x; }
    static Value Dict_()                { Value x; x.tag = Tag::Dict;
                                          x.dict = std::make_shared<DictObj>();
                                          return x; }
    static Value Func(std::shared_ptr<FunctionObj> f) {
        Value x; x.tag = Tag::Func; x.func = std::move(f); return x;
    }
    static Value Bltin(BuiltinFn fn)    { Value x; x.tag = Tag::Builtin; x.builtin = fn; return x; }
    static Value Cls(std::shared_ptr<ClassObj> c) {
        Value x; x.tag = Tag::Class; x.cls = std::move(c); return x;
    }
    static Value Inst(std::shared_ptr<InstanceObj> inst) {
        Value x; x.tag = Tag::Instance; x.inst = std::move(inst); return x;
    }
    static Value BMeth(std::shared_ptr<BoundMethodObj> bm) {
        Value x; x.tag = Tag::BoundMethod; x.meth = std::move(bm); return x;
    }

    // ---- introspection ---------------------------------------------------
    bool        is_truthy() const;
    std::string repr() const;
    std::string to_str() const;
    bool        equals(const Value& other) const;
    std::string type_name() const;
};

// --- Heap object definitions (Value is now complete) ------------------------

struct ListObj { std::vector<Value> items; };
struct DictObj { std::vector<std::pair<Value, Value>> entries; };  // ordered, linear-scan

struct FunctionObj {
    std::string              name;
    std::vector<std::string> params;
    std::vector<StmtPtr>*    body;        // borrowed from the AST
    std::shared_ptr<Env>     closure;
};

struct ClassObj {
    std::string                                                   name;
    std::shared_ptr<ClassObj>                                     base;
    std::unordered_map<std::string, std::shared_ptr<FunctionObj>> methods;
};

struct InstanceObj {
    std::shared_ptr<ClassObj>              cls;
    std::unordered_map<std::string, Value> attrs;
};

// A bound method captures a receiver and a callable. For class methods,
// `target` is an Instance and `fn` is a FunctionObj. For built-in methods
// such as list.qo'sh, `target` carries the receiver and `fn_native` is
// non-null — invoked with the receiver passed as `args[0]`.
struct BoundMethodObj {
    Value                        target;
    std::shared_ptr<FunctionObj> fn;          // user-defined method, OR
    BuiltinFn                    fn_native;   // native built-in method
    std::string                  method_name; // for repr()
};


inline bool Value::is_truthy() const {
    switch (tag) {
        case Tag::Nil:   return false;
        case Tag::Bool:  return b;
        case Tag::Int:   return i != 0;
        case Tag::Float: return d != 0.0;
        case Tag::Str:   return s && !s->empty();
        case Tag::List:  return list && !list->items.empty();
        case Tag::Dict:  return dict && !dict->entries.empty();
        default:         return true;
    }
}

inline std::string Value::type_name() const {
    switch (tag) {
        case Tag::Nil:         return "Bo\u02BBsh";
        case Tag::Bool:        return "mantiqiy";
        case Tag::Int:         return "butun";
        case Tag::Float:       return "suzuvchi";
        case Tag::Str:         return "satr";
        case Tag::List:        return "ro'yxat";
        case Tag::Dict:        return "lug'at";
        case Tag::Func:        return "funksiya";
        case Tag::Builtin:     return "ichki_funksiya";
        case Tag::Class:       return "sinf";
        case Tag::Instance:    return inst && inst->cls ? inst->cls->name : "obyekt";
        case Tag::BoundMethod: return "metod";
    }
    return "?";
}

// Format a double the way most users expect: trailing .0 for integers,
// no scientific noise for everyday magnitudes.
inline std::string format_float(double d) {
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d < 0 ? "-cheksiz" : "cheksiz";
    std::ostringstream os;
    if (d == static_cast<int64_t>(d) && std::abs(d) < 1e16) {
        os << static_cast<int64_t>(d) << ".0";
    } else {
        os.precision(15);
        os << d;
    }
    return os.str();
}

// Python-style string repr with escape sequences for unprintables.
inline std::string repr_string(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    bool has_single = s.find('\'') != std::string::npos;
    bool has_double = s.find('"')  != std::string::npos;
    char q = (has_single && !has_double) ? '"' : '\'';
    out.push_back(q);
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if      (c == '\\')          out += "\\\\";
        else if (c == q)             { out.push_back('\\'); out.push_back(q); }
        else if (c == '\n')          out += "\\n";
        else if (c == '\t')          out += "\\t";
        else if (c == '\r')          out += "\\r";
        else if (c < 0x20)           {
            char buf[6]; std::snprintf(buf, sizeof(buf), "\\x%02X", c);
            out += buf;
        }
        else                         out.push_back(static_cast<char>(c));
    }
    out.push_back(q);
    return out;
}


inline std::string Value::repr() const {
    switch (tag) {
        case Tag::Nil:   return "Bo\u02BBsh";
        case Tag::Bool:  return b ? "Rost" : "Yolg\u02BBon";
        case Tag::Int:   return std::to_string(i);
        case Tag::Float: return format_float(d);
        case Tag::Str:   return repr_string(s ? *s : std::string{});
        case Tag::List: {
            std::string out = "[";
            if (list) for (size_t k = 0; k < list->items.size(); ++k) {
                if (k) out += ", ";
                out += list->items[k].repr();
            }
            out += "]";
            return out;
        }
        case Tag::Dict: {
            std::string out = "{";
            if (dict) for (size_t k = 0; k < dict->entries.size(); ++k) {
                if (k) out += ", ";
                out += dict->entries[k].first.repr();
                out += ": ";
                out += dict->entries[k].second.repr();
            }
            out += "}";
            return out;
        }
        case Tag::Func:        return "<funksiya " + (func ? func->name : "?") + ">";
        case Tag::Builtin:     return "<ichki_funksiya>";
        case Tag::Class:       return "<sinf " + (cls ? cls->name : "?") + ">";
        case Tag::Instance:    return "<" + (inst && inst->cls ? inst->cls->name : "obyekt") + ">";
        case Tag::BoundMethod: return "<metod " + (meth ? meth->method_name : "?") + ">";
    }
    return "<?>";
}

inline std::string Value::to_str() const {
    if (tag == Tag::Str) return s ? *s : std::string{};
    return repr();
}

inline bool Value::equals(const Value& other) const {
    // Numeric equality across Int/Float/Bool.
    auto numeric = [&](const Value& v, double& out) -> bool {
        if (v.tag == Tag::Int)   { out = static_cast<double>(v.i); return true; }
        if (v.tag == Tag::Float) { out = v.d;                       return true; }
        if (v.tag == Tag::Bool)  { out = v.b ? 1.0 : 0.0;           return true; }
        return false;
    };
    double a, c;
    if (numeric(*this, a) && numeric(other, c)) return a == c;

    if (tag != other.tag) return false;
    switch (tag) {
        case Tag::Nil:   return true;
        case Tag::Str:   return (s ? *s : "") == (other.s ? *other.s : "");
        case Tag::List: {
            if (list.get() == other.list.get()) return true;
            if (!list || !other.list) return false;
            if (list->items.size() != other.list->items.size()) return false;
            for (size_t k = 0; k < list->items.size(); ++k)
                if (!list->items[k].equals(other.list->items[k])) return false;
            return true;
        }
        case Tag::Dict: {
            if (dict.get() == other.dict.get()) return true;
            return false;   // dict equality not required for v1
        }
        default: return false;
    }
}


// =============================================================================
// SECTION 8 — Environment (lexical scope chain)
// =============================================================================
//
// Each scope owns a hash map of names -> Values plus a pointer to the parent
// scope. Closures capture an environment by shared_ptr; this keeps the chain
// alive for as long as any function references it.

class Env : public std::enable_shared_from_this<Env> {
public:
    std::unordered_map<std::string, Value> vars;
    std::shared_ptr<Env>                   parent;

    Env() = default;
    explicit Env(std::shared_ptr<Env> p) : parent(std::move(p)) {}

    // Look up a name, walking the chain. Throws if not found.
    Value get(const std::string& name, Pos pos) const {
        const Env* cur = this;
        while (cur) {
            auto it = cur->vars.find(name);
            if (it != cur->vars.end()) return it->second;
            cur = cur->parent.get();
        }
        throw UzbekError("NomXatoligi", "'" + name + "' nomi aniqlanmagan", pos);
    }

    // Define / overwrite a name in this scope only.
    void set_local(const std::string& name, Value v) {
        vars[name] = std::move(v);
    }

    // Walk the chain and assign in-place to the first scope that has *name*.
    // If no scope already binds it, create a new binding in the current scope
    // — Python-like semantics for assignment at the top level of a block.
    void assign(const std::string& name, Value v) {
        Env* cur = this;
        while (cur) {
            auto it = cur->vars.find(name);
            if (it != cur->vars.end()) { it->second = std::move(v); return; }
            cur = cur->parent.get();
        }
        vars[name] = std::move(v);
    }
};


// =============================================================================
// SECTION 9 — Built-in functions and methods
// =============================================================================

[[noreturn]] inline void runtime_error(const std::string& kind, const std::string& msg, Pos p) {
    throw UzbekError(kind, msg, p);
}

namespace bi {

// yoz(*args)  — print, space-separated, newline-terminated.
inline Value yoz(std::vector<Value>& args, Pos /*pos*/) {
    for (size_t k = 0; k < args.size(); ++k) {
        if (k) std::cout << ' ';
        std::cout << args[k].to_str();
    }
    std::cout << '\n';
    return Value::Nil();
}

// kirit([prompt])  — read a line from stdin.
inline Value kirit(std::vector<Value>& args, Pos pos) {
    if (args.size() > 1)
        runtime_error("TurXatoligi", "kirit() ko'pi bilan bitta argument oladi", pos);
    if (!args.empty()) std::cout << args[0].to_str() << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return Value::Str("");
    return Value::Str(std::move(line));
}

// uzunlik(x)  — len for strings (codepoints), lists, dicts.
inline Value uzunlik(std::vector<Value>& args, Pos pos) {
    if (args.size() != 1)
        runtime_error("TurXatoligi", "uzunlik() bitta argument oladi", pos);
    const Value& v = args[0];
    switch (v.tag) {
        case Value::Tag::Str:  return Value::I(static_cast<int64_t>(u8::length_codepoints(*v.s)));
        case Value::Tag::List: return Value::I(static_cast<int64_t>(v.list->items.size()));
        case Value::Tag::Dict: return Value::I(static_cast<int64_t>(v.dict->entries.size()));
        default:
            runtime_error("TurXatoligi",
                "uzunlik() '" + v.type_name() + "' turi uchun mos kelmaydi", pos);
    }
}

// oraliq(stop) | oraliq(start, stop) | oraliq(start, stop, step)  — like range,
// but materialised as a list to keep iteration semantics simple.
inline Value oraliq(std::vector<Value>& args, Pos pos) {
    auto as_int = [&](const Value& v) -> int64_t {
        if (v.tag == Value::Tag::Int)  return v.i;
        if (v.tag == Value::Tag::Bool) return v.b ? 1 : 0;
        runtime_error("TurXatoligi", "oraliq() butun argumentlar kutadi", pos);
    };
    int64_t start = 0, stop = 0, step = 1;
    if      (args.size() == 1) { stop = as_int(args[0]); }
    else if (args.size() == 2) { start = as_int(args[0]); stop = as_int(args[1]); }
    else if (args.size() == 3) { start = as_int(args[0]); stop = as_int(args[1]); step = as_int(args[2]); }
    else runtime_error("TurXatoligi", "oraliq() 1..3 ta argument oladi", pos);
    if (step == 0) runtime_error("QiymatXatoligi", "oraliq() qadami nol bo'lishi mumkin emas", pos);
    Value out = Value::List_();
    if (step > 0) for (int64_t k = start; k < stop; k += step) out.list->items.push_back(Value::I(k));
    else          for (int64_t k = start; k > stop; k += step) out.list->items.push_back(Value::I(k));
    return out;
}


// type(x)  — return the type name as a string.
inline Value type_(std::vector<Value>& args, Pos pos) {
    if (args.size() != 1)
        runtime_error("TurXatoligi", "type() bitta argument oladi", pos);
    return Value::Str(args[0].type_name());
}

// str(x), int(x), float(x)  — conversions.
inline Value to_str_b(std::vector<Value>& args, Pos pos) {
    if (args.size() != 1) runtime_error("TurXatoligi", "satr() bitta argument oladi", pos);
    return Value::Str(args[0].to_str());
}
inline Value to_int_b(std::vector<Value>& args, Pos pos) {
    if (args.size() != 1) runtime_error("TurXatoligi", "butun() bitta argument oladi", pos);
    const Value& v = args[0];
    if (v.tag == Value::Tag::Int) return v;
    if (v.tag == Value::Tag::Bool) return Value::I(v.b ? 1 : 0);
    if (v.tag == Value::Tag::Float) return Value::I(static_cast<int64_t>(v.d));
    if (v.tag == Value::Tag::Str) {
        try { return Value::I(std::stoll(*v.s)); }
        catch (...) { runtime_error("QiymatXatoligi", "butun() satrini o'qib bo'lmadi", pos); }
    }
    runtime_error("TurXatoligi", "butun() '" + v.type_name() + "' turini qabul qilmaydi", pos);
}
inline Value to_float_b(std::vector<Value>& args, Pos pos) {
    if (args.size() != 1) runtime_error("TurXatoligi", "suzuvchi() bitta argument oladi", pos);
    const Value& v = args[0];
    if (v.tag == Value::Tag::Float) return v;
    if (v.tag == Value::Tag::Int)   return Value::F(static_cast<double>(v.i));
    if (v.tag == Value::Tag::Bool)  return Value::F(v.b ? 1.0 : 0.0);
    if (v.tag == Value::Tag::Str) {
        try { return Value::F(std::stod(*v.s)); }
        catch (...) { runtime_error("QiymatXatoligi", "suzuvchi() satrini o'qib bo'lmadi", pos); }
    }
    runtime_error("TurXatoligi", "suzuvchi() '" + v.type_name() + "' turini qabul qilmaydi", pos);
}

// list-method:  lst.qo'sh(x) -> lst.append(x). Bound at attribute lookup.
inline Value list_qosh(std::vector<Value>& args, Pos pos) {
    if (args.size() != 2)   // args[0] is the bound receiver
        runtime_error("TurXatoligi", "qo'sh() bitta argument oladi", pos);
    Value& self = args[0];
    if (self.tag != Value::Tag::List)
        runtime_error("TurXatoligi", "qo'sh() faqat ro'yxat ustida ishlaydi", pos);
    self.list->items.push_back(args[1]);
    return Value::Nil();
}

} // namespace bi

// Build the global environment of an interpreter. Public so the REPL can
// reuse a single Env across multiple inputs.
inline std::shared_ptr<Env> make_global_env() {
    auto g = std::make_shared<Env>();
    g->set_local("yoz",       Value::Bltin(bi::yoz));
    g->set_local("kirit",     Value::Bltin(bi::kirit));
    g->set_local("uzunlik",   Value::Bltin(bi::uzunlik));
    g->set_local("oraliq",    Value::Bltin(bi::oraliq));
    g->set_local("type",      Value::Bltin(bi::type_));
    g->set_local("satr",      Value::Bltin(bi::to_str_b));
    g->set_local("butun",     Value::Bltin(bi::to_int_b));
    g->set_local("suzuvchi",  Value::Bltin(bi::to_float_b));
    return g;
}


// =============================================================================
// SECTION 10 — Interpreter (tree-walking evaluator)
// =============================================================================
//
// Control-flow signals are raised as C++ exceptions so they can propagate
// out of arbitrarily deep call stacks. They are *internal* — never caught by
// user code — and never escape the interpreter boundary.

struct ReturnSignal   { Value value; };
struct BreakSignal    {};
struct ContinueSignal {};

class Interpreter {
public:
    Interpreter() : globals_(make_global_env()) {}

    void run(std::vector<StmtPtr>& program);
    Value eval(Expr* e, std::shared_ptr<Env> env);
    void  exec(Stmt* s, std::shared_ptr<Env> env);

    std::shared_ptr<Env> globals() const { return globals_; }

private:
    Value call_value(Value& callee, std::vector<Value> args, Pos pos);
    Value call_function(std::shared_ptr<FunctionObj> fn, std::vector<Value> args, Pos pos);
    Value get_attr(Value& target, const std::string& name, Pos pos);
    void  set_attr(Value& target, const std::string& name, Value v, Pos pos);
    Value get_index(Value& target, const Value& idx, Pos pos);
    void  set_index(Value& target, const Value& idx, Value v, Pos pos);

    Value binary_op(Tok op, const Value& l, const Value& r, Pos pos);
    Value compare_op(Tok op, const Value& l, const Value& r, Pos pos);
    bool  membership(const Value& l, const Value& container, Pos pos);

    void exec_assign(AssignStmt* s, std::shared_ptr<Env> env);
    void exec_aug_assign(AugAssignStmt* s, std::shared_ptr<Env> env);

    std::shared_ptr<Env> globals_;
};

inline void Interpreter::run(std::vector<StmtPtr>& program) {
    for (auto& stmt : program) exec(stmt.get(), globals_);
}


// --- Expression evaluation ---------------------------------------------------

inline Value Interpreter::eval(Expr* e, std::shared_ptr<Env> env) {
    switch (e->kind) {
        case Expr::Kind::Int:   return Value::I(static_cast<IntLit*>(e)->value);
        case Expr::Kind::Float: return Value::F(static_cast<FloatLit*>(e)->value);
        case Expr::Kind::Str:   return Value::Str(static_cast<StrLit*>(e)->value);
        case Expr::Kind::Bool:  return Value::B(static_cast<BoolLit*>(e)->value);
        case Expr::Kind::None:  return Value::Nil();
        case Expr::Kind::Name:  return env->get(static_cast<NameExpr*>(e)->name, e->pos);

        case Expr::Kind::Binary: {
            auto* n = static_cast<BinaryExpr*>(e);
            Value l = eval(n->left.get(), env);
            Value r = eval(n->right.get(), env);
            return binary_op(n->op, l, r, n->pos);
        }
        case Expr::Kind::Compare: {
            auto* n = static_cast<CompareExpr*>(e);
            Value l = eval(n->left.get(), env);
            Value r = eval(n->right.get(), env);
            return compare_op(n->op, l, r, n->pos);
        }
        case Expr::Kind::Membership: {
            auto* n = static_cast<MembershipExpr*>(e);
            Value l = eval(n->left.get(), env);
            Value c = eval(n->container.get(), env);
            bool m = membership(l, c, n->pos);
            return Value::B(n->negate ? !m : m);
        }
        case Expr::Kind::Logical: {
            auto* n = static_cast<LogicalExpr*>(e);
            Value l = eval(n->left.get(), env);
            if (n->op == Tok::KwYoki) return l.is_truthy() ? l : eval(n->right.get(), env);
            else                      return l.is_truthy() ? eval(n->right.get(), env) : l;
        }
        case Expr::Kind::Unary: {
            auto* n = static_cast<UnaryExpr*>(e);
            Value v = eval(n->operand.get(), env);
            switch (n->op) {
                case Tok::Minus:
                    if (v.tag == Value::Tag::Int)   return Value::I(-v.i);
                    if (v.tag == Value::Tag::Float) return Value::F(-v.d);
                    if (v.tag == Value::Tag::Bool)  return Value::I(-(v.b ? 1 : 0));
                    runtime_error("TurXatoligi", "'-' operatori '" + v.type_name() + "' turini qabul qilmaydi", n->pos);
                case Tok::Plus:
                    if (v.tag == Value::Tag::Int || v.tag == Value::Tag::Float) return v;
                    runtime_error("TurXatoligi", "'+' operatori '" + v.type_name() + "' turini qabul qilmaydi", n->pos);
                case Tok::KwEmas: return Value::B(!v.is_truthy());
                default:           runtime_error("TizimXatoligi", "noma'lum unar operator", n->pos);
            }
        }
        case Expr::Kind::List: {
            auto* n = static_cast<ListExpr*>(e);
            Value out = Value::List_();
            for (auto& el : n->elements) out.list->items.push_back(eval(el.get(), env));
            return out;
        }
        case Expr::Kind::Dict: {
            auto* n = static_cast<DictExpr*>(e);
            Value out = Value::Dict_();
            for (auto& kv : n->items) {
                Value k = eval(kv.first.get(), env);
                Value v = eval(kv.second.get(), env);
                bool found = false;
                for (auto& en : out.dict->entries) {
                    if (en.first.equals(k)) { en.second = v; found = true; break; }
                }
                if (!found) out.dict->entries.emplace_back(std::move(k), std::move(v));
            }
            return out;
        }
        case Expr::Kind::Index: {
            auto* n = static_cast<IndexExpr*>(e);
            Value t = eval(n->target.get(), env);
            Value i = eval(n->index.get(), env);
            return get_index(t, i, n->pos);
        }
        case Expr::Kind::Attr: {
            auto* n = static_cast<AttrExpr*>(e);
            Value t = eval(n->target.get(), env);
            return get_attr(t, n->name, n->pos);
        }
        case Expr::Kind::Call: {
            auto* n = static_cast<CallExpr*>(e);
            Value callee = eval(n->callee.get(), env);
            std::vector<Value> args;
            args.reserve(n->args.size());
            for (auto& a : n->args) args.push_back(eval(a.get(), env));
            return call_value(callee, std::move(args), n->pos);
        }
    }
    return Value::Nil();
}


// --- Arithmetic & comparison -------------------------------------------------

inline Value Interpreter::binary_op(Tok op, const Value& l, const Value& r, Pos pos) {
    // String concatenation (+) and repetition (str * int)
    if (op == Tok::Plus && l.tag == Value::Tag::Str && r.tag == Value::Tag::Str) {
        return Value::Str(*l.s + *r.s);
    }
    if (op == Tok::Plus && l.tag == Value::Tag::List && r.tag == Value::Tag::List) {
        Value out = Value::List_();
        out.list->items = l.list->items;
        out.list->items.insert(out.list->items.end(), r.list->items.begin(), r.list->items.end());
        return out;
    }
    if (op == Tok::Star && l.tag == Value::Tag::Str && r.tag == Value::Tag::Int) {
        std::string out;
        if (r.i > 0) { out.reserve(l.s->size() * static_cast<size_t>(r.i));
                       for (int64_t k = 0; k < r.i; ++k) out += *l.s; }
        return Value::Str(std::move(out));
    }
    if (op == Tok::Star && l.tag == Value::Tag::List && r.tag == Value::Tag::Int) {
        Value out = Value::List_();
        for (int64_t k = 0; k < r.i; ++k)
            out.list->items.insert(out.list->items.end(), l.list->items.begin(), l.list->items.end());
        return out;
    }

    // Numeric promotion to double when either side is float; otherwise int math.
    auto numeric = [&](const Value& v, double& out) {
        if (v.tag == Value::Tag::Int)   { out = static_cast<double>(v.i); return true; }
        if (v.tag == Value::Tag::Float) { out = v.d; return true; }
        if (v.tag == Value::Tag::Bool)  { out = v.b ? 1.0 : 0.0; return true; }
        return false;
    };

    double la, ra;
    if (!numeric(l, la) || !numeric(r, ra))
        runtime_error("TurXatoligi",
            "amal '" + std::string(tok_name(op)) + "' '" + l.type_name() +
            "' va '" + r.type_name() + "' turlariga mos kelmaydi", pos);

    bool both_int = (l.tag == Value::Tag::Int  || l.tag == Value::Tag::Bool) &&
                    (r.tag == Value::Tag::Int  || r.tag == Value::Tag::Bool);

    switch (op) {
        case Tok::Plus:    return both_int ? Value::I(static_cast<int64_t>(la) + static_cast<int64_t>(ra))
                                           : Value::F(la + ra);
        case Tok::Minus:   return both_int ? Value::I(static_cast<int64_t>(la) - static_cast<int64_t>(ra))
                                           : Value::F(la - ra);
        case Tok::Star:    return both_int ? Value::I(static_cast<int64_t>(la) * static_cast<int64_t>(ra))
                                           : Value::F(la * ra);
        case Tok::Slash:
            if (ra == 0.0) runtime_error("NolgaBo\u02BBlishXatoligi", "nolga bo'lish mumkin emas", pos);
            return Value::F(la / ra);
        case Tok::DoubleSlash:
            if (ra == 0.0) runtime_error("NolgaBo\u02BBlishXatoligi", "nolga bo'lish mumkin emas", pos);
            if (both_int) return Value::I(static_cast<int64_t>(std::floor(la / ra)));
            return Value::F(std::floor(la / ra));
        case Tok::Percent:
            if (ra == 0.0) runtime_error("NolgaBo\u02BBlishXatoligi", "nolga bo'lish mumkin emas", pos);
            if (both_int) {
                int64_t a = static_cast<int64_t>(la), b = static_cast<int64_t>(ra);
                int64_t m = a % b;
                if ((m != 0) && ((m < 0) != (b < 0))) m += b;
                return Value::I(m);
            }
            return Value::F(la - std::floor(la / ra) * ra);
        case Tok::StarStar:
            if (both_int && ra >= 0) {
                int64_t base = static_cast<int64_t>(la), e = static_cast<int64_t>(ra), out = 1;
                while (e > 0) { if (e & 1) out *= base; base *= base; e >>= 1; }
                return Value::I(out);
            }
            return Value::F(std::pow(la, ra));
        default:
            runtime_error("TizimXatoligi", "noma'lum binar operator", pos);
    }
}


inline Value Interpreter::compare_op(Tok op, const Value& l, const Value& r, Pos pos) {
    if (op == Tok::Eq)    return Value::B(l.equals(r));
    if (op == Tok::NotEq) return Value::B(!l.equals(r));

    auto numeric = [&](const Value& v, double& out) {
        if (v.tag == Value::Tag::Int)   { out = static_cast<double>(v.i); return true; }
        if (v.tag == Value::Tag::Float) { out = v.d; return true; }
        if (v.tag == Value::Tag::Bool)  { out = v.b ? 1.0 : 0.0; return true; }
        return false;
    };
    double la, ra;
    if (numeric(l, la) && numeric(r, ra)) {
        switch (op) {
            case Tok::Lt: return Value::B(la <  ra);
            case Tok::Gt: return Value::B(la >  ra);
            case Tok::Le: return Value::B(la <= ra);
            case Tok::Ge: return Value::B(la >= ra);
            default:      break;
        }
    }
    if (l.tag == Value::Tag::Str && r.tag == Value::Tag::Str) {
        const std::string& a = *l.s; const std::string& b = *r.s;
        switch (op) {
            case Tok::Lt: return Value::B(a <  b);
            case Tok::Gt: return Value::B(a >  b);
            case Tok::Le: return Value::B(a <= b);
            case Tok::Ge: return Value::B(a >= b);
            default:      break;
        }
    }
    runtime_error("TurXatoligi",
        "taqqoslash '" + l.type_name() + "' va '" + r.type_name() + "' turlariga mos kelmaydi", pos);
}

inline bool Interpreter::membership(const Value& l, const Value& container, Pos pos) {
    switch (container.tag) {
        case Value::Tag::List:
            for (auto& it : container.list->items) if (it.equals(l)) return true;
            return false;
        case Value::Tag::Dict:
            for (auto& kv : container.dict->entries) if (kv.first.equals(l)) return true;
            return false;
        case Value::Tag::Str:
            if (l.tag != Value::Tag::Str)
                runtime_error("TurXatoligi", "satr ichida faqat satr qidirish mumkin", pos);
            return container.s->find(*l.s) != std::string::npos;
        default:
            runtime_error("TurXatoligi",
                "'ichida' '" + container.type_name() + "' turi bilan ishlamaydi", pos);
    }
}


// --- Index / attr ------------------------------------------------------------

inline Value Interpreter::get_index(Value& target, const Value& idx, Pos pos) {
    if (target.tag == Value::Tag::List) {
        if (idx.tag != Value::Tag::Int)
            runtime_error("TurXatoligi", "ro'yxat indeksi butun bo'lishi shart", pos);
        int64_t k = idx.i;
        int64_t n = static_cast<int64_t>(target.list->items.size());
        if (k < 0) k += n;
        if (k < 0 || k >= n)
            runtime_error("IndeksXatoligi", "ro'yxat indeksi diapazondan tashqarida", pos);
        return target.list->items[static_cast<size_t>(k)];
    }
    if (target.tag == Value::Tag::Dict) {
        for (auto& kv : target.dict->entries)
            if (kv.first.equals(idx)) return kv.second;
        runtime_error("KalitXatoligi", "lug'atda kalit topilmadi: " + idx.repr(), pos);
    }
    if (target.tag == Value::Tag::Str) {
        if (idx.tag != Value::Tag::Int)
            runtime_error("TurXatoligi", "satr indeksi butun bo'lishi shart", pos);
        // Codepoint-aware indexing. O(n), but we don't expect huge strings here.
        const char* p = target.s->data();
        const char* end = p + target.s->size();
        int64_t k = idx.i;
        int64_t total = static_cast<int64_t>(u8::length_codepoints(*target.s));
        if (k < 0) k += total;
        if (k < 0 || k >= total)
            runtime_error("IndeksXatoligi", "satr indeksi diapazondan tashqarida", pos);
        for (int64_t i = 0; i < k; ++i) u8::decode(p, end);
        const char* save = p;
        uint32_t cp = u8::decode(p, end);
        std::string out;
        u8::encode(out, cp);
        (void)save;
        return Value::Str(std::move(out));
    }
    runtime_error("TurXatoligi", "'" + target.type_name() + "' turini indekslab bo'lmaydi", pos);
}

inline void Interpreter::set_index(Value& target, const Value& idx, Value v, Pos pos) {
    if (target.tag == Value::Tag::List) {
        if (idx.tag != Value::Tag::Int)
            runtime_error("TurXatoligi", "ro'yxat indeksi butun bo'lishi shart", pos);
        int64_t k = idx.i;
        int64_t n = static_cast<int64_t>(target.list->items.size());
        if (k < 0) k += n;
        if (k < 0 || k >= n)
            runtime_error("IndeksXatoligi", "ro'yxat indeksi diapazondan tashqarida", pos);
        target.list->items[static_cast<size_t>(k)] = std::move(v);
        return;
    }
    if (target.tag == Value::Tag::Dict) {
        for (auto& kv : target.dict->entries) {
            if (kv.first.equals(idx)) { kv.second = std::move(v); return; }
        }
        target.dict->entries.emplace_back(idx, std::move(v));
        return;
    }
    runtime_error("TurXatoligi", "'" + target.type_name() + "' turi indeks bo'yicha o'zgarmaydi", pos);
}


inline Value Interpreter::get_attr(Value& target, const std::string& name, Pos pos) {
    // Instance attribute lookup: instance dict, then class chain (with method binding).
    if (target.tag == Value::Tag::Instance) {
        auto it = target.inst->attrs.find(name);
        if (it != target.inst->attrs.end()) return it->second;
        std::shared_ptr<ClassObj> cls = target.inst->cls;
        while (cls) {
            auto m = cls->methods.find(name);
            if (m != cls->methods.end()) {
                auto bm = std::make_shared<BoundMethodObj>();
                bm->target = target;
                bm->fn = m->second;
                bm->fn_native = nullptr;
                bm->method_name = name;
                return Value::BMeth(bm);
            }
            cls = cls->base;
        }
        runtime_error("AtributXatoligi",
            "'" + target.inst->cls->name + "' obyektida '" + name + "' atributi yo'q", pos);
    }
    // Class attribute lookup: methods only (used to implement classmethod-ish access).
    if (target.tag == Value::Tag::Class) {
        std::shared_ptr<ClassObj> cls = target.cls;
        while (cls) {
            auto m = cls->methods.find(name);
            if (m != cls->methods.end()) return Value::Func(m->second);
            cls = cls->base;
        }
        runtime_error("AtributXatoligi",
            "'" + target.cls->name + "' sinfida '" + name + "' atributi yo'q", pos);
    }
    // Built-in methods on container types.
    if (target.tag == Value::Tag::List && name == "qo'sh") {
        auto bm = std::make_shared<BoundMethodObj>();
        bm->target = target; bm->fn = nullptr; bm->fn_native = bi::list_qosh;
        bm->method_name = "qo'sh";
        return Value::BMeth(bm);
    }
    runtime_error("AtributXatoligi",
        "'" + target.type_name() + "' turida '" + name + "' atributi yo'q", pos);
}

inline void Interpreter::set_attr(Value& target, const std::string& name, Value v, Pos pos) {
    if (target.tag == Value::Tag::Instance) {
        target.inst->attrs[name] = std::move(v);
        return;
    }
    runtime_error("TurXatoligi",
        "'" + target.type_name() + "' turida atribut o'rnatib bo'lmaydi", pos);
}


// --- Calls -------------------------------------------------------------------

inline Value Interpreter::call_value(Value& callee, std::vector<Value> args, Pos pos) {
    switch (callee.tag) {
        case Value::Tag::Builtin:
            return callee.builtin(args, pos);

        case Value::Tag::Func:
            return call_function(callee.func, std::move(args), pos);

        case Value::Tag::Class: {
            // Construct an instance, optionally invoke __init__.
            auto inst = std::make_shared<InstanceObj>();
            inst->cls = callee.cls;
            Value v = Value::Inst(inst);
            // Look up __init__ along the class chain.
            std::shared_ptr<FunctionObj> init;
            std::shared_ptr<ClassObj> cls = callee.cls;
            while (cls) {
                auto it = cls->methods.find("__init__");
                if (it != cls->methods.end()) { init = it->second; break; }
                cls = cls->base;
            }
            if (init) {
                std::vector<Value> a;
                a.reserve(args.size() + 1);
                a.push_back(v);
                for (auto& x : args) a.push_back(std::move(x));
                call_function(init, std::move(a), pos);
            } else if (!args.empty()) {
                runtime_error("TurXatoligi",
                    "'" + callee.cls->name + "' sinfining __init__() yo'q, argument qabul qilmaydi", pos);
            }
            return v;
        }

        case Value::Tag::BoundMethod: {
            auto bm = callee.meth;
            std::vector<Value> a;
            a.reserve(args.size() + 1);
            a.push_back(bm->target);
            for (auto& x : args) a.push_back(std::move(x));
            if (bm->fn_native) return bm->fn_native(a, pos);
            return call_function(bm->fn, std::move(a), pos);
        }

        default:
            runtime_error("TurXatoligi",
                "'" + callee.type_name() + "' turidagi obyektni chaqirib bo'lmaydi", pos);
    }
}

inline Value Interpreter::call_function(std::shared_ptr<FunctionObj> fn,
                                        std::vector<Value> args, Pos pos) {
    if (args.size() != fn->params.size()) {
        std::ostringstream os;
        os << "'" << fn->name << "' funksiyasi " << fn->params.size()
           << " ta argument kutadi, lekin " << args.size() << " ta berildi";
        runtime_error("TurXatoligi", os.str(), pos);
    }
    auto env = std::make_shared<Env>(fn->closure);
    for (size_t k = 0; k < fn->params.size(); ++k) {
        env->set_local(fn->params[k], std::move(args[k]));
    }
    try {
        for (auto& st : *fn->body) exec(st.get(), env);
    } catch (ReturnSignal& sig) {
        return std::move(sig.value);
    }
    return Value::Nil();
}


// --- Statement execution -----------------------------------------------------

inline void Interpreter::exec_assign(AssignStmt* s, std::shared_ptr<Env> env) {
    Value v = eval(s->value.get(), env);
    Expr* tgt = s->target.get();
    switch (tgt->kind) {
        case Expr::Kind::Name:
            env->assign(static_cast<NameExpr*>(tgt)->name, std::move(v));
            return;
        case Expr::Kind::Index: {
            auto* ix = static_cast<IndexExpr*>(tgt);
            Value t = eval(ix->target.get(), env);
            Value i = eval(ix->index.get(), env);
            set_index(t, i, std::move(v), ix->pos);
            return;
        }
        case Expr::Kind::Attr: {
            auto* a = static_cast<AttrExpr*>(tgt);
            Value t = eval(a->target.get(), env);
            set_attr(t, a->name, std::move(v), a->pos);
            return;
        }
        default:
            runtime_error("TizimXatoligi", "noto'g'ri o'zlashtirish maqsadi", s->pos);
    }
}

inline void Interpreter::exec_aug_assign(AugAssignStmt* s, std::shared_ptr<Env> env) {
    // Compute old value, apply the binary op, then assign back.
    Tok bop;
    switch (s->op) {
        case Tok::PlusEq:    bop = Tok::Plus;        break;
        case Tok::MinusEq:   bop = Tok::Minus;       break;
        case Tok::StarEq:    bop = Tok::Star;        break;
        case Tok::SlashEq:   bop = Tok::Slash;       break;
        case Tok::DSlashEq:  bop = Tok::DoubleSlash; break;
        case Tok::PercentEq: bop = Tok::Percent;     break;
        default:
            runtime_error("TizimXatoligi", "noma'lum kengaytirilgan o'zlashtirish", s->pos);
    }
    Expr* tgt = s->target.get();
    Value rhs = eval(s->value.get(), env);
    if (tgt->kind == Expr::Kind::Name) {
        Value cur = env->get(static_cast<NameExpr*>(tgt)->name, tgt->pos);
        Value nv = binary_op(bop, cur, rhs, s->pos);
        env->assign(static_cast<NameExpr*>(tgt)->name, std::move(nv));
        return;
    }
    if (tgt->kind == Expr::Kind::Index) {
        auto* ix = static_cast<IndexExpr*>(tgt);
        Value t = eval(ix->target.get(), env);
        Value i = eval(ix->index.get(), env);
        Value cur = get_index(t, i, ix->pos);
        Value nv = binary_op(bop, cur, rhs, s->pos);
        set_index(t, i, std::move(nv), ix->pos);
        return;
    }
    if (tgt->kind == Expr::Kind::Attr) {
        auto* a = static_cast<AttrExpr*>(tgt);
        Value t = eval(a->target.get(), env);
        Value cur = get_attr(t, a->name, a->pos);
        Value nv = binary_op(bop, cur, rhs, s->pos);
        set_attr(t, a->name, std::move(nv), a->pos);
        return;
    }
    runtime_error("TizimXatoligi", "noto'g'ri kengaytirilgan o'zlashtirish maqsadi", s->pos);
}


inline void Interpreter::exec(Stmt* s, std::shared_ptr<Env> env) {
    switch (s->kind) {
        case Stmt::Kind::ExprS: {
            auto* e = static_cast<ExprStmt*>(s);
            eval(e->expr.get(), env);
            return;
        }
        case Stmt::Kind::Assign:    exec_assign(static_cast<AssignStmt*>(s), env);     return;
        case Stmt::Kind::AugAssign: exec_aug_assign(static_cast<AugAssignStmt*>(s), env); return;

        case Stmt::Kind::If: {
            auto* n = static_cast<IfStmt*>(s);
            if (eval(n->cond.get(), env).is_truthy()) {
                for (auto& st : n->then_body) exec(st.get(), env);
            } else {
                bool taken = false;
                for (auto& el : n->elifs) {
                    if (eval(el.first.get(), env).is_truthy()) {
                        for (auto& st : el.second) exec(st.get(), env);
                        taken = true; break;
                    }
                }
                if (!taken && n->has_else) {
                    for (auto& st : n->else_body) exec(st.get(), env);
                }
            }
            return;
        }
        case Stmt::Kind::While: {
            auto* n = static_cast<WhileStmt*>(s);
            while (eval(n->cond.get(), env).is_truthy()) {
                try {
                    for (auto& st : n->body) exec(st.get(), env);
                } catch (BreakSignal&)    { break; }
                catch (ContinueSignal&) { continue; }
            }
            return;
        }
        case Stmt::Kind::For: {
            auto* n = static_cast<ForStmt*>(s);
            Value iter = eval(n->iter.get(), env);
            if (iter.tag != Value::Tag::List && iter.tag != Value::Tag::Str &&
                iter.tag != Value::Tag::Dict) {
                runtime_error("TurXatoligi",
                    "'" + iter.type_name() + "' iteratsiya qilinmaydi", n->pos);
            }
            auto step_body = [&](Value item) {
                env->assign(n->var, std::move(item));
                try {
                    for (auto& st : n->body) exec(st.get(), env);
                } catch (BreakSignal&)    { throw; }
                catch (ContinueSignal&) { /* fall through */ }
            };
            try {
                if (iter.tag == Value::Tag::List) {
                    for (auto& it : iter.list->items) step_body(it);
                } else if (iter.tag == Value::Tag::Dict) {
                    for (auto& kv : iter.dict->entries) step_body(kv.first);
                } else { // string — iterate codepoints
                    const char* p = iter.s->data();
                    const char* end = p + iter.s->size();
                    while (p < end) {
                        const char* save = p;
                        uint32_t cp = u8::decode(p, end);
                        std::string ch;
                        u8::encode(ch, cp);
                        (void)save;
                        step_body(Value::Str(std::move(ch)));
                    }
                }
            } catch (BreakSignal&) { /* loop terminated */ }
            return;
        }
        case Stmt::Kind::FuncDef: {
            auto* n = static_cast<FuncDefStmt*>(s);
            auto fn = std::make_shared<FunctionObj>();
            fn->name = n->name;
            fn->params = n->params;
            fn->body = &n->body;
            fn->closure = env;
            env->set_local(n->name, Value::Func(fn));
            return;
        }
        case Stmt::Kind::ClassDef: {
            auto* n = static_cast<ClassDefStmt*>(s);
            auto cls = std::make_shared<ClassObj>();
            cls->name = n->name;
            if (!n->base.empty()) {
                Value base = env->get(n->base, n->pos);
                if (base.tag != Value::Tag::Class)
                    runtime_error("TurXatoligi", "ota '" + n->base + "' sinf emas", n->pos);
                cls->base = base.cls;
            }
            for (auto& m : n->methods) {
                auto fn = std::make_shared<FunctionObj>();
                fn->name = m->name;
                fn->params = m->params;
                fn->body = &m->body;
                fn->closure = env;
                cls->methods[m->name] = std::move(fn);
            }
            env->set_local(n->name, Value::Cls(cls));
            return;
        }
        case Stmt::Kind::Return: {
            auto* n = static_cast<ReturnStmt*>(s);
            Value v = n->value ? eval(n->value.get(), env) : Value::Nil();
            throw ReturnSignal{ std::move(v) };
        }
        case Stmt::Kind::Break:    throw BreakSignal{};
        case Stmt::Kind::Continue: throw ContinueSignal{};
        case Stmt::Kind::Pass:     return;
    }
}


// =============================================================================
// SECTION 11 — AST debug printer (used by `--ast`)
// =============================================================================

inline void dump_expr(Expr* e, int indent, std::ostream& out);
inline void dump_stmt(Stmt* s, int indent, std::ostream& out);

inline void pad(int n, std::ostream& out) { for (int i = 0; i < n; ++i) out << "  "; }

inline void dump_expr(Expr* e, int indent, std::ostream& out) {
    pad(indent, out);
    switch (e->kind) {
        case Expr::Kind::Int:   out << "Int("   << static_cast<IntLit*>(e)->value   << ")\n"; return;
        case Expr::Kind::Float: out << "Float(" << static_cast<FloatLit*>(e)->value << ")\n"; return;
        case Expr::Kind::Str:   out << "Str("   << repr_string(static_cast<StrLit*>(e)->value) << ")\n"; return;
        case Expr::Kind::Bool:  out << "Bool("  << (static_cast<BoolLit*>(e)->value ? "Rost" : "Yolg\u02BBon") << ")\n"; return;
        case Expr::Kind::None:  out << "Bo\u02BBsh\n"; return;
        case Expr::Kind::Name:  out << "Name(" << static_cast<NameExpr*>(e)->name << ")\n"; return;
        case Expr::Kind::Binary: {
            auto* n = static_cast<BinaryExpr*>(e);
            out << "Binary(" << tok_name(n->op) << ")\n";
            dump_expr(n->left.get(),  indent + 1, out);
            dump_expr(n->right.get(), indent + 1, out);
            return;
        }
        case Expr::Kind::Compare: {
            auto* n = static_cast<CompareExpr*>(e);
            out << "Compare(" << tok_name(n->op) << ")\n";
            dump_expr(n->left.get(),  indent + 1, out);
            dump_expr(n->right.get(), indent + 1, out);
            return;
        }
        case Expr::Kind::Membership: {
            auto* n = static_cast<MembershipExpr*>(e);
            out << "Ichida\n";
            dump_expr(n->left.get(),      indent + 1, out);
            dump_expr(n->container.get(), indent + 1, out);
            return;
        }
        case Expr::Kind::Logical: {
            auto* n = static_cast<LogicalExpr*>(e);
            out << "Logical(" << tok_name(n->op) << ")\n";
            dump_expr(n->left.get(),  indent + 1, out);
            dump_expr(n->right.get(), indent + 1, out);
            return;
        }
        case Expr::Kind::Unary: {
            auto* n = static_cast<UnaryExpr*>(e);
            out << "Unary(" << tok_name(n->op) << ")\n";
            dump_expr(n->operand.get(), indent + 1, out);
            return;
        }
        case Expr::Kind::Call: {
            auto* n = static_cast<CallExpr*>(e);
            out << "Call\n";
            dump_expr(n->callee.get(), indent + 1, out);
            for (auto& a : n->args) dump_expr(a.get(), indent + 1, out);
            return;
        }
        case Expr::Kind::Index: {
            auto* n = static_cast<IndexExpr*>(e);
            out << "Index\n";
            dump_expr(n->target.get(), indent + 1, out);
            dump_expr(n->index.get(),  indent + 1, out);
            return;
        }
        case Expr::Kind::Attr: {
            auto* n = static_cast<AttrExpr*>(e);
            out << "Attr(" << n->name << ")\n";
            dump_expr(n->target.get(), indent + 1, out);
            return;
        }
        case Expr::Kind::List: {
            auto* n = static_cast<ListExpr*>(e);
            out << "List(" << n->elements.size() << ")\n";
            for (auto& el : n->elements) dump_expr(el.get(), indent + 1, out);
            return;
        }
        case Expr::Kind::Dict: {
            auto* n = static_cast<DictExpr*>(e);
            out << "Dict(" << n->items.size() << ")\n";
            for (auto& kv : n->items) {
                dump_expr(kv.first.get(),  indent + 1, out);
                dump_expr(kv.second.get(), indent + 2, out);
            }
            return;
        }
    }
}


inline void dump_stmt(Stmt* s, int indent, std::ostream& out) {
    pad(indent, out);
    switch (s->kind) {
        case Stmt::Kind::ExprS: {
            out << "ExprStmt\n";
            dump_expr(static_cast<ExprStmt*>(s)->expr.get(), indent + 1, out);
            return;
        }
        case Stmt::Kind::Assign: {
            auto* n = static_cast<AssignStmt*>(s);
            out << "Assign\n";
            dump_expr(n->target.get(), indent + 1, out);
            dump_expr(n->value.get(),  indent + 1, out);
            return;
        }
        case Stmt::Kind::AugAssign: {
            auto* n = static_cast<AugAssignStmt*>(s);
            out << "AugAssign(" << tok_name(n->op) << ")\n";
            dump_expr(n->target.get(), indent + 1, out);
            dump_expr(n->value.get(),  indent + 1, out);
            return;
        }
        case Stmt::Kind::If: {
            auto* n = static_cast<IfStmt*>(s);
            out << "If\n";
            pad(indent + 1, out); out << "cond:\n";
            dump_expr(n->cond.get(), indent + 2, out);
            pad(indent + 1, out); out << "then:\n";
            for (auto& b : n->then_body) dump_stmt(b.get(), indent + 2, out);
            for (auto& el : n->elifs) {
                pad(indent + 1, out); out << "elif:\n";
                dump_expr(el.first.get(), indent + 2, out);
                for (auto& b : el.second) dump_stmt(b.get(), indent + 2, out);
            }
            if (n->has_else) {
                pad(indent + 1, out); out << "else:\n";
                for (auto& b : n->else_body) dump_stmt(b.get(), indent + 2, out);
            }
            return;
        }
        case Stmt::Kind::While: {
            auto* n = static_cast<WhileStmt*>(s);
            out << "While\n";
            dump_expr(n->cond.get(), indent + 1, out);
            for (auto& b : n->body) dump_stmt(b.get(), indent + 1, out);
            return;
        }
        case Stmt::Kind::For: {
            auto* n = static_cast<ForStmt*>(s);
            out << "For(" << n->var << ")\n";
            dump_expr(n->iter.get(), indent + 1, out);
            for (auto& b : n->body) dump_stmt(b.get(), indent + 1, out);
            return;
        }
        case Stmt::Kind::FuncDef: {
            auto* n = static_cast<FuncDefStmt*>(s);
            out << "FuncDef(" << n->name << ", params=";
            for (size_t k = 0; k < n->params.size(); ++k) {
                if (k) out << ",";
                out << n->params[k];
            }
            out << ")\n";
            for (auto& b : n->body) dump_stmt(b.get(), indent + 1, out);
            return;
        }
        case Stmt::Kind::ClassDef: {
            auto* n = static_cast<ClassDefStmt*>(s);
            out << "ClassDef(" << n->name;
            if (!n->base.empty()) out << " : " << n->base;
            out << ")\n";
            for (auto& m : n->methods) dump_stmt(m.get(), indent + 1, out);
            return;
        }
        case Stmt::Kind::Return: {
            auto* n = static_cast<ReturnStmt*>(s);
            out << "Return\n";
            if (n->value) dump_expr(n->value.get(), indent + 1, out);
            return;
        }
        case Stmt::Kind::Break:    out << "Break\n";    return;
        case Stmt::Kind::Continue: out << "Continue\n"; return;
        case Stmt::Kind::Pass:     out << "Pass\n";     return;
    }
}


// =============================================================================
// SECTION 12 — CLI dispatch (file mode, -c, --tokens, --ast, REPL)
// =============================================================================

inline std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "FaylTopilmadiXatoligi: '" << path << "' faylini ochib bo'lmadi\n";
        std::exit(2);
    }
    std::ostringstream os;
    os << in.rdbuf();
    return os.str();
}

inline int run_source(const std::string& source, const std::string& filename) {
    try {
        Lexer lex(source, filename);
        std::vector<Token> toks = lex.tokenize();
        Parser parser(std::move(toks), filename);
        std::vector<StmtPtr> program = parser.parse_program();
        Interpreter interp;
        interp.run(program);
    } catch (UzbekError& err) {
        std::cerr << err.render(source) << '\n';
        return 1;
    }
    return 0;
}

inline int dump_tokens(const std::string& source, const std::string& filename) {
    try {
        Lexer lex(source, filename);
        for (const Token& t : lex.tokenize()) {
            std::cout << t.pos.line << ":" << t.pos.col << "  "
                      << tok_name(t.type);
            if (t.type == Tok::Int)        std::cout << "  " << t.int_val;
            else if (t.type == Tok::Float) std::cout << "  " << format_float(t.float_val);
            else if (t.type == Tok::String) std::cout << "  " << repr_string(t.str_val);
            else if (!t.text.empty() && t.type != Tok::Newline) std::cout << "  " << t.text;
            std::cout << '\n';
        }
    } catch (UzbekError& err) {
        std::cerr << err.render(source) << '\n';
        return 1;
    }
    return 0;
}

inline int dump_ast(const std::string& source, const std::string& filename) {
    try {
        Lexer lex(source, filename);
        Parser parser(lex.tokenize(), filename);
        std::vector<StmtPtr> program = parser.parse_program();
        for (auto& s : program) dump_stmt(s.get(), 0, std::cout);
    } catch (UzbekError& err) {
        std::cerr << err.render(source) << '\n';
        return 1;
    }
    return 0;
}


inline int repl() {
    std::cout << "Uzbek-PY native runtime " << "1.0.0" << "  -  Pythonsiz, mustaqil.\n";
    std::cout << "Chiqish uchun  Ctrl-D  yoki  oxir  yozing.\n";

    Interpreter interp;
    auto env = interp.globals();

    // Functions defined in the REPL hold raw pointers into the AST that
    // produced them, so we must keep every parsed program alive for the
    // lifetime of the session — otherwise the next iteration's pointers
    // dangle the moment a previous batch's vector goes out of scope.
    std::vector<std::vector<StmtPtr>> live_programs;
    auto run_chunk = [&](const std::string& src) {
        try {
            Lexer lex(src, "<repl>");
            Parser parser(lex.tokenize(), "<repl>");
            std::vector<StmtPtr> prog = parser.parse_program();
            live_programs.push_back(std::move(prog));
            auto& batch = live_programs.back();
            for (auto& st : batch) {
                if (st->kind == Stmt::Kind::ExprS) {
                    Value v = interp.eval(static_cast<ExprStmt*>(st.get())->expr.get(), env);
                    if (v.tag != Value::Tag::Nil) std::cout << v.repr() << '\n';
                } else {
                    interp.exec(st.get(), env);
                }
            }
        } catch (UzbekError& err) {
            std::cerr << err.render(src) << '\n';
        }
    };

    std::string buffer;
    while (true) {
        std::cout << (buffer.empty() ? "uz> " : "..> ") << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) { std::cout << "\n"; break; }
        if (buffer.empty() && (line == "oxir" || line == "exit" || line == "quit")) break;

        if (buffer.empty()) {
            std::string trimmed = line;
            while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) trimmed.pop_back();
            bool needs_more = !trimmed.empty() && (trimmed.back() == ':' || trimmed.back() == '\\');
            if (!needs_more) {
                run_chunk(line + "\n");
                continue;
            }
            buffer = line + "\n";
            continue;
        }

        if (line.empty()) {
            std::string src = buffer;
            buffer.clear();
            run_chunk(src);
            continue;
        }
        buffer += line + "\n";
    }

    // Break the env <-> function shared_ptr cycle before the interpreter
    // (and its globals) go out of scope, so destruction is deterministic.
    env->vars.clear();
    return 0;
}


} // namespace uzbek

// =============================================================================
// SECTION 13 — main
// =============================================================================

static void print_usage(std::ostream& out) {
    out <<
        "Foydalanish:\n"
        "  uzbek file.uz                  - .uz faylini ishga tushirish\n"
        "  uzbek -c 'yoz(\"Salom!\")'       - bir qatorli kodni ishga tushirish\n"
        "  uzbek --tokens file.uz         - leksik tokenlarni ko'rsatish\n"
        "  uzbek --ast    file.uz         - sintaksis daraxtini ko'rsatish\n"
        "  uzbek                          - interaktiv REPL\n"
        "  uzbek --version                - versiya ma'lumotini ko'rsatish\n"
        "  uzbek -h | --help              - shu yordamni ko'rsatish\n";
}

int main(int argc, char** argv) {
    if (argc <= 1) return uzbek::repl();

    std::string a1 = argv[1];

    if (a1 == "-h" || a1 == "--help")    { print_usage(std::cout); return 0; }
    if (a1 == "--version" || a1 == "-v") { std::cout << "Uzbek-PY native 1.0.0\n"; return 0; }

    if (a1 == "-c") {
        if (argc < 3) { std::cerr << "Xato: -c keyin kod kutilgan edi\n"; return 2; }
        return uzbek::run_source(std::string(argv[2]) + "\n", "<command>");
    }

    if (a1 == "--tokens") {
        if (argc < 3) { std::cerr << "Xato: --tokens keyin fayl kutilgan edi\n"; return 2; }
        return uzbek::dump_tokens(uzbek::read_file(argv[2]), argv[2]);
    }
    if (a1 == "--ast") {
        if (argc < 3) { std::cerr << "Xato: --ast keyin fayl kutilgan edi\n"; return 2; }
        return uzbek::dump_ast(uzbek::read_file(argv[2]), argv[2]);
    }

    if (!a1.empty() && a1[0] == '-') {
        std::cerr << "Xato: noma'lum bayroq '" << a1 << "'\n";
        print_usage(std::cerr);
        return 2;
    }

    return uzbek::run_source(uzbek::read_file(a1), a1);
}
