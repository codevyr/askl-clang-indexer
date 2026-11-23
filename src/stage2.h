#pragma once

#include "stage1.h" // for RefData

#include <clang-c/Index.h>
#include <functional>
#include <string>
#include <vector>

class SymbolTable;

/// A place in the code: a file and a byte range in it.  See
/// `relation_sites.h`.
struct Stage2Place {
    bool known = false;
    std::string file;
    int32_t start = 0, end = 0;
};

struct Stage2Ref {
    std::string source_file;
    RefData data;
    /// For a relation row (`RefData::relation`): the place that made it so,
    /// which is not where the row is.  The row sits over the field's
    /// declaration; the place is the initialiser or the assignment that
    /// stored something in the field.
    Stage2Place made_at;
};

struct Stage2Result {
    std::vector<Stage2Ref> refs;
};

class Stage2 {
public:
    /// Does Stage1 place a reference to the symbol `name` for this cursor, at
    /// a token that names it?  See `Stage2::addFuncPtrRef`.
    using PlacedByStage1 = std::function<bool(CXCursor, const std::string& name)>;
    /// Where the macro invocation that starts at this offset of this file
    /// ends, or -1 when none starts there.  See `Stage2::placeOf`.
    using InvocationEndAt = std::function<int32_t(const std::string& path, int32_t start)>;

    Stage2(SymbolTable& symbols, PlacedByStage1 placed_by_stage1,
           InvocationEndAt invocation_end_at = nullptr);

    void process(CXTranslationUnit tu, const std::string& tu_filename);
    Stage2Result takeResults();

private:
    SymbolTable& symbols_;
    PlacedByStage1 placed_by_stage1_;
    InvocationEndAt invocation_end_at_;
    Stage2Result result_;
    std::string tu_filename_;

    static CXChildVisitResult visitor(CXCursor cursor, CXCursor parent, CXClientData data);
    void visitCursor(CXCursor cursor, CXCursor parent);
    void handleInitList(CXCursor cursor);
    void handleInitEntry(CXCursor child, int field_index, const std::vector<CXCursor>& fields);
    void handleBinaryAssignment(CXCursor cursor);
    void addFuncPtrRef(CXCursor func_ref, CXFile source_file, unsigned start_off, unsigned end_off);
    /// `made_at`: the whole initialiser or assignment that stores it
    /// (`.read = my_read`, `ops->read = my_read`), so whoever is sent there
    /// sees the slot, the `=` and what was stored.
    void addFieldImplRef(CXCursor member_ref, CXCursor func_ref, const Stage2Place& made_at);
    void addFieldImplRefFromDecl(CXCursor field_decl, CXCursor func_ref,
                                 const Stage2Place& made_at);
    void addFieldToFieldRef(CXCursor lhs_member_ref, CXCursor rhs_member_ref,
                            const Stage2Place& made_at);
    /// The place the text of `cursor` occupies in the file it was typed in.
    Stage2Place placeOf(CXCursor cursor) const;
};
