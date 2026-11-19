#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

/// WHERE THE CALLER'S PART OF A PASTED NAME WAS TYPED.
///
/// `CALL(send(x))` with `#define CALL(a) mca_pml.pml_ ## a` names the field
/// `pml_send`, and no token anywhere spells it: the preprocessor builds it,
/// and clang keeps it in a scratch buffer with no file location.  All libclang
/// will say about it is where the outermost invocation starts.  It does not
/// say that `send`, at the call site, is the half that picked this field.
///
/// That half is recoverable from tokens, which libclang does give: the
/// invocation's (with file offsets) and each `#define`'s.  So the `##` is
/// replayed on them -- parameters bound to the tokens passed for them, each
/// paste joined, nested invocations followed -- carrying along where every
/// token was typed.  The result is a map from each identifier the expansion
/// builds to the place its chosen part was written.
///
/// "Chosen" means it reached the `##` THROUGH A PARAMETER.  A name pasted from
/// two halves the macro wrote itself (`pml_ ## send`) was chosen by nobody,
/// is absent from the map, and stays the macro's own reference.
///
/// TWO PLACES, because a name can be chosen in two steps.  An X-macro list
///
///     #define EACH(_m)   _m(alpha) _m(beta)
///     #define ENTRY(_n)  handle_ ## _n,
///     int (*table[])(int) = { EACH(ENTRY) };
///
/// types `alpha` in its body, but that alone names nothing: `EACH(DECLARE)`
/// would make declarations of the same list and `EACH(STRINGIFY)` no symbols
/// at all.  What makes `handle_alpha` a reference HERE is `ENTRY`, the macro
/// passed in and applied by the list.  So the reference SITS on `ENTRY`, at
/// the call site, and the edge is `table -> handle_alpha` -- while the part
/// of the name was TYPED in the list's body, which is what says whether
/// anyone wrote this name in normal code.  Both are reported.
///
/// It follows the preprocessor's rules as far as pasting depends on them: an
/// argument beside a `##` is used as written and anywhere else is expanded
/// first, which is what makes the two-level `CONCAT` idiom work; a macro is
/// not expanded inside its own expansion; `, ## __VA_ARGS__` is not a paste.
/// It is not a preprocessor beyond that.  An inner macro is resolved by name
/// alone, and an expansion is not rescanned together with the text after it.
/// Where the replay cannot follow, the name is simply not in the map and the
/// caller falls back to what it did before.

/// Token spellings, interned.
///
/// A replay moves tokens around far more than it looks at them -- every
/// substitution copies its argument, at every level of nesting -- and asks
/// "is this a macro" of each identifier each time.  With the spelling an id,
/// a token is a few plain words and that question is an array index.  The
/// kernel is why it matters: a slice of 398 translation units replays 300,000
/// invocations.
class PasteStrings {
public:
    using Id = uint32_t;

    PasteStrings();

    Id intern(const std::string& text);
    /// The id of `text` if it has been seen, without adding it.
    bool find(const std::string& text, Id& out) const;
    const std::string& str(Id id) const { return strings_[id]; }
    size_t size() const { return strings_.size(); }

    // The punctuation the replay acts on.
    Id lparen, rparen, comma, hash, hashhash, ellipsis, va_args, empty_string;

private:
    std::vector<std::string> strings_;
    std::unordered_map<std::string, Id> ids_;
};

struct PasteOrigin {
    uint32_t file_idx = 0;
    int32_t start = -1;
    int32_t end = -1;
    bool valid() const { return start >= 0; }
};

/// One name an expansion pastes together.
struct PasteSite {
    /// Where the reference to it sits: the macro argument that completed the
    /// name when a macro passed as an argument did the pasting, otherwise the
    /// same place as `typed`.
    PasteOrigin site;
    /// Where the part of the name a caller chose was typed.
    PasteOrigin typed;
    /// The range at `site` spells a macro that builds the name, not a part
    /// of the name itself.
    bool by_callback = false;
};

struct PasteToken {
    PasteStrings::Id text = 0;
    bool ident = false;
    // --- replay state -----------------------------------------------------
    /// Substituted for a parameter, as opposed to written in the body.
    bool from_param = false;
    /// Built by `##`.
    bool pasted = false;
    /// Where this token was typed.  Invalid for one the replay built.
    PasteOrigin origin;
    /// For a pasted token: where the part a caller chose was typed.
    PasteOrigin chosen;
};

struct PasteMacro {
    PasteStrings::Id name = 0;
    bool function_like = false;
    /// Parameter names; a bare `...` is `__VA_ARGS__`.
    std::vector<PasteStrings::Id> params;
    /// The last parameter takes every remaining argument.
    bool variadic = false;
    std::vector<PasteToken> body;
};

/// Resolve a macro by the id of its name, or nullptr.  Called for identifiers
/// met while expanding; the pointer must stay valid for the whole replay.
using PasteLookup = std::function<const PasteMacro*(PasteStrings::Id)>;

/// Replay one invocation of `macro`.  `after_name` is the invocation's tokens
/// following the macro's name -- `( send ( x ) )` -- or empty for an
/// object-like macro.
///
/// Returns pasted identifier -> where it sits and where its chosen part was
/// typed.  When the same identifier is built more than once, the first wins.
std::unordered_map<PasteStrings::Id, PasteSite>
replayPastes(const PasteMacro& macro, const std::vector<PasteToken>& after_name,
             PasteStrings& strings, const PasteLookup& lookup);
