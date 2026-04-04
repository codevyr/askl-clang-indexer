#pragma once

#include "stage1.h" // for RefData

#include <clang-c/Index.h>
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
    explicit Stage2(SymbolTable& symbols);

    void process(CXTranslationUnit tu, const std::string& tu_filename);
    Stage2Result takeResults();

private:
    SymbolTable& symbols_;
    Stage2Result result_;
    std::string tu_filename_;

    static CXChildVisitResult visitor(CXCursor cursor, CXCursor parent, CXClientData data);
    void visitCursor(CXCursor cursor, CXCursor parent);
    void handleDesignatedInit(CXCursor cursor);
    void handleBinaryAssignment(CXCursor cursor);
    void addFuncPtrRef(CXCursor func_ref, CXFile source_file, unsigned start_off, unsigned end_off);
    void addFieldImplRef(CXCursor member_ref, CXCursor func_ref);
};
