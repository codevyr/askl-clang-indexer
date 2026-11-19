#include "macro_paste.h"

#include <algorithm>

PasteStrings::PasteStrings() {
    lparen = intern("(");
    rparen = intern(")");
    comma = intern(",");
    hash = intern("#");
    hashhash = intern("##");
    ellipsis = intern("...");
    va_args = intern("__VA_ARGS__");
    empty_string = intern("\"\"");
}

PasteStrings::Id PasteStrings::intern(const std::string& text) {
    auto it = ids_.find(text);
    if (it != ids_.end()) return it->second;
    const Id id = static_cast<Id>(strings_.size());
    strings_.push_back(text);
    ids_.emplace(text, id);
    return id;
}

bool PasteStrings::find(const std::string& text, Id& out) const {
    auto it = ids_.find(text);
    if (it == ids_.end()) return false;
    out = it->second;
    return true;
}

namespace {

using Tokens = std::vector<PasteToken>;
using Id = PasteStrings::Id;

bool isIdentifier(const std::string& s) {
    if (s.empty()) return false;
    auto head = [](unsigned char c) {
        return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    if (!head(static_cast<unsigned char>(s[0]))) return false;
    for (unsigned char c : s)
        if (!head(c) && !(c >= '0' && c <= '9')) return false;
    return true;
}

/// The part of `t` a caller chose, if any: already decided for a token built
/// by an earlier paste, otherwise the token itself when a parameter brought
/// it here.
PasteOrigin chosenPart(const PasteToken& t) {
    if (t.chosen.valid()) return t.chosen;
    if (t.from_param) return t.origin;
    return {};
}

class Replay {
public:
    Replay(PasteStrings& strings, const PasteLookup& lookup,
           std::unordered_map<Id, PasteSite>& found)
        : strings_(strings), lookup_(lookup), found_(found) {}

    /// Replace every macro invocation in `in` by its expansion, recording the
    /// pastes met on the way.
    Tokens expandAll(const Tokens& in) {
        Tokens out;
        out.reserve(in.size());
        for (size_t i = 0; i < in.size();) {
            const PasteToken& t = in[i];
            const PasteMacro* macro = t.ident ? lookup_(t.text) : nullptr;
            size_t at = i + 1;
            std::vector<Tokens> args;
            if (!macro || !enter(*macro) ||
                (macro->function_like && !readArgs(in, at, args))) {
                out.push_back(t);
                ++i;
                continue;
            }
            // A MACRO PASSED IN AND APPLIED HERE: its name came through a
            // parameter and its argument list did not -- `_m(alpha)` in a
            // list's body.  (Both coming through a parameter is just an
            // invocation the caller wrote inside an argument.)
            const bool callback = macro->function_like && t.from_param && t.origin.valid() &&
                                  !in[i + 1].from_param;
            if (callback) callbacks_.push_back(t.origin);
            Tokens expansion = invoke(*macro, args);
            if (callback) callbacks_.pop_back();
            out.insert(out.end(), expansion.begin(), expansion.end());
            i = at;
        }
        return out;
    }

    /// One invocation: substitute, paste, and expand what that leaves.
    Tokens invoke(const PasteMacro& macro, const std::vector<Tokens>& args) {
        active_.push_back(&macro);
        Tokens out = expandAll(substitute(macro, args));
        active_.pop_back();
        return out;
    }

    /// Read `( a , b )` starting at `tokens[at]`, splitting on the commas that
    /// are not inside nested parentheses.  Leaves `at` past the closing `)`.
    bool readArgs(const Tokens& tokens, size_t& at, std::vector<Tokens>& args) const {
        if (at >= tokens.size() || tokens[at].text != strings_.lparen) return false;
        int depth = 0;
        Tokens cur;
        for (size_t i = at; i < tokens.size(); ++i) {
            const Id t = tokens[i].text;
            if (t == strings_.lparen) {
                if (depth++ > 0) cur.push_back(tokens[i]);
            } else if (t == strings_.rparen) {
                if (--depth == 0) {
                    args.push_back(std::move(cur));
                    at = i + 1;
                    return true;
                }
                cur.push_back(tokens[i]);
            } else if (t == strings_.comma && depth == 1) {
                args.push_back(std::move(cur));
                cur.clear();
            } else {
                cur.push_back(tokens[i]);
            }
        }
        return false; // unbalanced: not an invocation we can read
    }

private:
    // A replay is bounded on both axes.  Neither limit is reached by ordinary
    // code; they exist so that a pathological macro costs a missing reference
    // rather than a hung indexer.
    static constexpr size_t kMaxDepth = 64;
    static constexpr size_t kMaxTokens = 200000;

    PasteStrings& strings_;
    const PasteLookup& lookup_;
    std::unordered_map<Id, PasteSite>& found_;
    /// The macro arguments currently being applied as macros, innermost last;
    /// see `PasteSite`.
    std::vector<PasteOrigin> callbacks_;
    std::vector<const PasteMacro*> active_;
    size_t spent_ = 0;

    /// May `macro` be expanded here?  Not inside its own expansion -- the
    /// preprocessor's rule, and what makes a self-referential definition
    /// terminate -- and not once the replay has run out of room.
    bool enter(const PasteMacro& macro) const {
        if (spent_ > kMaxTokens || active_.size() >= kMaxDepth) return false;
        return std::find(active_.begin(), active_.end(), &macro) == active_.end();
    }

    PasteToken paste(const PasteToken& lhs, const PasteToken& rhs) {
        const std::string text = strings_.str(lhs.text) + strings_.str(rhs.text);
        PasteToken out;
        out.text = strings_.intern(text);
        out.ident = isIdentifier(text);
        out.pasted = true;
        // Left first: for `a ## _ ## b` with both halves chosen, one place has
        // to be picked, and the first is as good as any and stable.
        out.chosen = chosenPart(lhs);
        if (!out.chosen.valid()) out.chosen = chosenPart(rhs);
        return out;
    }

    /// The tokens passed for each parameter.  The last parameter of a
    /// variadic macro takes every remaining argument, commas included.
    std::vector<Tokens> bind(const PasteMacro& macro, const std::vector<Tokens>& args) const {
        std::vector<Tokens> bound(macro.params.size());
        for (size_t p = 0; p < macro.params.size(); ++p) {
            if (macro.variadic && p + 1 == macro.params.size()) {
                for (size_t a = p; a < args.size(); ++a) {
                    if (a > p) {
                        PasteToken comma;
                        comma.text = strings_.comma;
                        bound[p].push_back(comma);
                    }
                    bound[p].insert(bound[p].end(), args[a].begin(), args[a].end());
                }
            } else if (p < args.size()) {
                bound[p] = args[p];
            }
        }
        return bound;
    }

    static void markFromParam(Tokens& tokens) {
        for (PasteToken& t : tokens) t.from_param = true;
    }

    Tokens substitute(const PasteMacro& macro, const std::vector<Tokens>& args) {
        const Tokens& body = macro.body;
        spent_ += body.size();

        auto paramIndex = [&](const PasteToken& t) -> int {
            if (!t.ident) return -1;
            for (size_t p = 0; p < macro.params.size(); ++p)
                if (macro.params[p] == t.text) return static_cast<int>(p);
            return -1;
        };

        // AN ARGUMENT IS USED IN ONE OF TWO FORMS, and which one is the whole
        // reason `CONCAT(a, b)` is always written through a second macro:
        // beside a `##` it is substituted exactly as written, anywhere else it
        // is fully expanded first.  So
        //
        //     #define CONCAT_(a, b) a ## b
        //     #define CONCAT(a, b)  CONCAT_(a, b)
        //
        // pastes the VALUE of `SUFFIX` in `CONCAT(name, SUFFIX)`, where
        // `CONCAT_` alone would paste the word.  Getting this wrong builds a
        // name that does not exist and finds nothing.
        std::vector<Tokens> raw = bind(macro, args);
        for (Tokens& r : raw) markFromParam(r);
        std::vector<Tokens> expanded(raw.size());
        std::vector<char> have_expanded(raw.size(), 0);
        auto expandedArg = [&](size_t p) -> const Tokens& {
            if (!have_expanded[p]) {
                have_expanded[p] = 1;
                expanded[p] = expandAll(raw[p]);
                markFromParam(expanded[p]);
            }
            return expanded[p];
        };

        Tokens out;
        out.reserve(body.size());
        // How many tokens the operand just emitted, so that `##` after an
        // EMPTY operand joins nothing rather than whatever came before it.
        size_t last_emitted = 0;
        for (size_t i = 0; i < body.size() && spent_ <= kMaxTokens; ++i) {
            const PasteToken& t = body[i];

            if (t.text == strings_.hashhash && i > 0 && i + 1 < body.size()) {
                const PasteToken& next = body[++i];
                const int p = paramIndex(next);
                const Tokens single{next};
                const Tokens& rhs = p >= 0 ? raw[static_cast<size_t>(p)] : single;
                spent_ += rhs.size();
                // `, ## __VA_ARGS__` is not a paste: it keeps the comma when
                // there are arguments and drops it when there are none.
                if (last_emitted > 0 && out.back().text == strings_.comma &&
                    !out.back().pasted) {
                    if (rhs.empty())
                        out.pop_back();
                    else
                        out.insert(out.end(), rhs.begin(), rhs.end());
                    last_emitted = rhs.size();
                    continue;
                }
                if (rhs.empty()) continue;      // left ## nothing: left stands
                if (last_emitted == 0) {        // nothing ## right: right stands
                    out.insert(out.end(), rhs.begin(), rhs.end());
                } else {
                    out.back() = paste(out.back(), rhs.front());
                    out.insert(out.end(), rhs.begin() + 1, rhs.end());
                }
                last_emitted = rhs.size();
                continue;
            }

            // `#param` makes a string.  Its contents are of no interest, only
            // that it is one token and not the parameter's.
            if (t.text == strings_.hash && macro.function_like && i + 1 < body.size() &&
                paramIndex(body[i + 1]) >= 0) {
                PasteToken str;
                str.text = strings_.empty_string;
                out.push_back(str);
                last_emitted = 1;
                ++i;
                continue;
            }

            const int p = paramIndex(t);
            if (p < 0) {
                out.push_back(t);
                last_emitted = 1;
                continue;
            }
            const bool pasted_next =
                i + 1 < body.size() && body[i + 1].text == strings_.hashhash;
            const Tokens& sub = pasted_next ? raw[static_cast<size_t>(p)]
                                            : expandedArg(static_cast<size_t>(p));
            spent_ += sub.size();
            out.insert(out.end(), sub.begin(), sub.end());
            last_emitted = sub.size();
        }

        for (const PasteToken& t : out) {
            if (!t.pasted || !t.ident || !t.chosen.valid()) continue;
            PasteSite found;
            found.typed = t.chosen;
            found.by_callback = !callbacks_.empty();
            found.site = found.by_callback ? callbacks_.back() : t.chosen;
            found_.emplace(t.text, found);
        }
        return out;
    }
};

} // namespace

std::unordered_map<PasteStrings::Id, PasteSite>
replayPastes(const PasteMacro& macro, const std::vector<PasteToken>& after_name,
             PasteStrings& strings, const PasteLookup& lookup) {
    std::unordered_map<PasteStrings::Id, PasteSite> found;
    Replay replay(strings, lookup, found);
    std::vector<Tokens> args;
    size_t at = 0;
    if (macro.function_like && !replay.readArgs(after_name, at, args)) return found;
    replay.invoke(macro, args);
    return found;
}
