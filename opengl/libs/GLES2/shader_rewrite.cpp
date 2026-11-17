/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "shader_rewrite.h"

#include <ctype.h>

#include <string_view>
#include <vector>

namespace android {
namespace gles {

namespace {

struct Token {
    std::string_view text;
    size_t begin;
    size_t end;
};

std::vector<Token> tokenize(const std::string& src) {
    std::vector<Token> out;
    const size_t n = src.size();
    size_t i = 0;
    while (i < n) {
        const char c = src[i];
        if (isspace(static_cast<unsigned char>(c))) {
            i++;
        } else if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') i++;
        } else if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            size_t e = src.find("*/", i + 2);
            i = (e == std::string::npos) ? n : e + 2;
        } else if (c == '#') {
            // Preprocessor directive, up to the end of the (possibly continued) line.
            while (i < n && src[i] != '\n') {
                if (src[i] == '\\' && i + 1 < n && src[i + 1] == '\n') i++;
                i++;
            }
        } else if (isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.') {
            size_t s = i;
            while (i < n &&
                   (isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_' || src[i] == '.')) {
                i++;
            }
            out.push_back({std::string_view(src).substr(s, i - s), s, i});
        } else {
            out.push_back({std::string_view(src).substr(i, 1), i, i + 1});
            i++;
        }
    }
    return out;
}

enum class Kind { Simple, Block, If, Loop, Do, Break, Continue };

struct Node {
    Kind kind;
    size_t first; // first token
    size_t last;  // one past the last token
    int a = -1;   // If: then-branch; Loop/Do: body
    int b = -1;   // If: else-branch
    std::vector<int> kids; // Block
    size_t whileTok = 0;   // Do: index of the `while` token
    bool falseCond = false; // Do: condition is literally `false`
};

class Parser {
public:
    Parser(const std::vector<Token>& toks, std::vector<Node>& nodes) : t(toks), nodes(nodes) {}

    // Parses one statement at token i; returns the node index or -1 on error.
    int statement(int depth) {
        if (depth > kMaxDepth || i >= t.size()) return -1;
        const size_t start = i;
        const std::string_view v = t[i].text;
        if (v == "{") {
            i++;
            std::vector<int> kids;
            while (i < t.size() && t[i].text != "}") {
                int k = statement(depth + 1);
                if (k < 0) return -1;
                kids.push_back(k);
            }
            if (!eat("}")) return -1;
            int n = add(Kind::Block, start);
            nodes[n].kids = std::move(kids);
            return n;
        }
        if (v == "if") {
            i++;
            if (!parens()) return -1;
            int a = statement(depth + 1);
            if (a < 0) return -1;
            int b = -1;
            if (i < t.size() && t[i].text == "else") {
                i++;
                b = statement(depth + 1);
                if (b < 0) return -1;
            }
            int n = add(Kind::If, start);
            nodes[n].a = a;
            nodes[n].b = b;
            return n;
        }
        if (v == "for" || v == "while" || v == "switch") {
            i++;
            if (!parens()) return -1;
            int body = statement(depth + 1);
            if (body < 0) return -1;
            int n = add(Kind::Loop, start);
            nodes[n].a = body;
            return n;
        }
        if (v == "do") {
            i++;
            int body = statement(depth + 1);
            if (body < 0) return -1;
            const size_t w = i;
            if (!eat("while")) return -1;
            const size_t c0 = i;
            if (!parens()) return -1;
            const bool isFalse = (i - c0 == 3) && t[c0 + 1].text == "false";
            if (!eat(";")) return -1;
            int n = add(Kind::Do, start);
            nodes[n].a = body;
            nodes[n].whileTok = w;
            nodes[n].falseCond = isFalse;
            return n;
        }
        if (v == "break" || v == "continue") {
            i++;
            if (!eat(";")) return -1;
            return add(v == "break" ? Kind::Break : Kind::Continue, start);
        }
        // Anything else (declarations, expressions, return, continue, discard,
        // case labels) runs to the next ';' outside parentheses/brackets.
        int d = 0;
        while (i < t.size()) {
            const std::string_view x = t[i++].text;
            if (x == "(" || x == "[") {
                d++;
            } else if (x == ")" || x == "]") {
                d--;
            } else if (x == ";" && d == 0) {
                return add(Kind::Simple, start);
            } else if ((x == "{" || x == "}") && d == 0) {
                return -1;
            }
        }
        return -1;
    }

    size_t i = 0;

private:
    static constexpr int kMaxDepth = 128;

    int add(Kind k, size_t start) {
        Node n;
        n.kind = k;
        n.first = start;
        n.last = i;
        nodes.push_back(std::move(n));
        return static_cast<int>(nodes.size() - 1);
    }
    bool eat(std::string_view v) {
        if (i >= t.size() || t[i].text != v) return false;
        i++;
        return true;
    }
    bool parens() {
        if (!eat("(")) return false;
        int d = 1;
        while (d > 0 && i < t.size()) {
            const std::string_view x = t[i++].text;
            if (x == "(") d++;
            if (x == ")") d--;
        }
        return d == 0;
    }

    const std::vector<Token>& t;
    std::vector<Node>& nodes;
};

// Does node contain a break or continue that targets the innermost enclosing do?
bool hasBreak(const std::vector<Node>& nodes, int idx) {
    const Node& n = nodes[idx];
    switch (n.kind) {
        case Kind::Break:
        case Kind::Continue:
            return true;
        case Kind::If:
            return hasBreak(nodes, n.a) || (n.b >= 0 && hasBreak(nodes, n.b));
        case Kind::Block:
            for (int k : n.kids) {
                if (hasBreak(nodes, k)) return true;
            }
            return false;
        default: // breaks inside nested loops/switches target those
            return false;
    }
}

// True if every break inside node that targets the enclosing do is in tail
// position and there is no continue targeting it; those breaks are appended to
// `breaks`.
bool tailBreaks(const std::vector<Node>& nodes, int idx, std::vector<int>& breaks) {
    const Node& n = nodes[idx];
    switch (n.kind) {
        case Kind::Break:
            breaks.push_back(idx);
            return true;
        case Kind::If:
            return tailBreaks(nodes, n.a, breaks) && (n.b < 0 || tailBreaks(nodes, n.b, breaks));
        case Kind::Block: {
            // Trailing breaks are all dead or tail; the statement before them is
            // the tail statement; everything earlier must not break.
            int last = static_cast<int>(n.kids.size()) - 1;
            while (last >= 0 && nodes[n.kids[last]].kind == Kind::Break) last--;
            for (int j = 0; j < static_cast<int>(n.kids.size()); j++) {
                const int k = n.kids[j];
                if (j < last) {
                    if (hasBreak(nodes, k)) return false;
                } else if (j == last) {
                    if (!tailBreaks(nodes, k, breaks)) return false;
                } else {
                    breaks.push_back(k);
                }
            }
            return true;
        }
        default:
            return !hasBreak(nodes, idx);
    }
}

struct Hit {
    int doNode;
    std::vector<int> breaks;
};

void collect(const std::vector<Node>& nodes, int idx, std::vector<Hit>& hits) {
    const Node& n = nodes[idx];
    switch (n.kind) {
        case Kind::Do:
            if (n.falseCond && nodes[n.a].kind == Kind::Block) {
                Hit h{idx, {}};
                if (tailBreaks(nodes, n.a, h.breaks)) hits.push_back(std::move(h));
            }
            collect(nodes, n.a, hits);
            break;
        case Kind::Loop:
            collect(nodes, n.a, hits);
            break;
        case Kind::If:
            collect(nodes, n.a, hits);
            if (n.b >= 0) collect(nodes, n.b, hits);
            break;
        case Kind::Block:
            for (int k : n.kids) collect(nodes, k, hits);
            break;
        default:
            break;
    }
}

} // namespace

size_t flattenTrivialDoWhile(std::string& src) {
    const std::vector<Token> t = tokenize(src);
    std::vector<Node> nodes;
    std::vector<Hit> hits;

    // Parse every function body: a top-level '{' that follows ')'.
    int depth = 0;
    for (size_t i = 0; i < t.size();) {
        const std::string_view v = t[i].text;
        if (v == "{" && depth == 0 && i > 0 && t[i - 1].text == ")") {
            Parser p(t, nodes);
            p.i = i;
            int body = p.statement(0);
            if (body < 0) return 0; // unexpected syntax: leave the source alone
            collect(nodes, body, hits);
            i = p.i;
            continue;
        }
        if (v == "{") depth++;
        if (v == "}") depth--;
        i++;
    }

    auto blank = [&](size_t firstTok, size_t lastTok) {
        for (size_t j = t[firstTok].begin; j < t[lastTok - 1].end; j++) {
            if (src[j] != '\n') src[j] = ' ';
        }
    };
    for (const Hit& h : hits) {
        const Node& d = nodes[h.doNode];
        blank(d.first, d.first + 1);  // do
        blank(d.whileTok, d.last);    // while (false);
        for (int b : h.breaks) blank(nodes[b].first, nodes[b].last);
    }
    return hits.size();
}

} // namespace gles
} // namespace android
