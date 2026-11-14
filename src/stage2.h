#pragma once

#include "stage1.h" // for RefData

#include <clang-c/Index.h>
#include <functional>
#include <string>
#include <vector>

class SymbolTable;

struct Stage2Ref {
    std::string source_file;
    RefData data;
};

struct Stage2Result {
    std::vector<Stage2Ref> refs;
};

class Stage2 {
public:
    /// Does Stage1 place a reference to the symbol `name` for this cursor, at
    /// a token that names it?  See `Stage2::addFuncPtrRef`.
    using PlacedByStage1 = std::function<bool(CXCursor, const std::string& name)>;

    Stage2(SymbolTable& symbols, PlacedByStage1 placed_by_stage1);

    void process(CXTranslationUnit tu, const std::string& tu_filename);
    Stage2Result takeResults();

private:
    SymbolTable& symbols_;
    PlacedByStage1 placed_by_stage1_;
    Stage2Result result_;
    std::string tu_filename_;

    static CXChildVisitResult visitor(CXCursor cursor, CXCursor parent, CXClientData data);
    void visitCursor(CXCursor cursor, CXCursor parent);
    void handleInitList(CXCursor cursor);
    void handleInitEntry(CXCursor child, int field_index, const std::vector<CXCursor>& fields);
    void handleBinaryAssignment(CXCursor cursor);
    void addFuncPtrRef(CXCursor func_ref, CXFile source_file, unsigned start_off, unsigned end_off);
    void addFieldImplRef(CXCursor member_ref, CXCursor func_ref);
    void addFieldImplRefFromDecl(CXCursor field_decl, CXCursor func_ref);
    void addFieldToFieldRef(CXCursor lhs_member_ref, CXCursor rhs_member_ref);
};
