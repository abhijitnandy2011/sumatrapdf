/* Copyright 2019 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#pragma warning(disable : 4611) // interaction between '_setjmp' and C++ object destruction is non-portable

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "utils/BaseUtil.h"
#include "utils/Archive.h"
#include "utils/ScopedWin.h"
#include "utils/FileUtil.h"
#include "utils/HtmlParserLookup.h"
#include "utils/HtmlPullParser.h"
#include "utils/TrivialHtmlParser.h"
#include "utils/WinUtil.h"
#include "utils/ZipUtil.h"
#include "utils/Log.h"

#include "AppColors.h"
#include "wingui/TreeModel.h"
#include "EngineBase.h"
#include "EngineFzUtil.h"
#include "EnginePdf.h"

// represents .vbkm file
struct VBkm {};

Kind kindEnginePdfMulti = "enginePdfMulti";

class EnginePdfMultiImpl : public EngineBase {
  public:
    EnginePdfMultiImpl();
    virtual ~EnginePdfMultiImpl();
    EngineBase* Clone() override;

    int PageCount() const override;

    RectD PageMediabox(int pageNo) override;
    RectD PageContentBox(int pageNo, RenderTarget target = RenderTarget::View) override;

    RenderedBitmap* RenderBitmap(int pageNo, float zoom, int rotation,
                                 RectD* pageRect = nullptr, /* if nullptr: defaults to the page's mediabox */
                                 RenderTarget target = RenderTarget::View, AbortCookie** cookie_out = nullptr) override;

    PointD Transform(PointD pt, int pageNo, float zoom, int rotation, bool inverse = false) override;
    RectD Transform(RectD rect, int pageNo, float zoom, int rotation, bool inverse = false) override;

    std::string_view GetFileData() override;
    bool SaveFileAs(const char* copyFileName, bool includeUserAnnots = false) override;
    bool SaveFileAsPdf(const char* pdfFileName, bool includeUserAnnots = false);
    WCHAR* ExtractPageText(int pageNo, RectI** coordsOut = nullptr) override;

    bool HasClipOptimizations(int pageNo) override;
    WCHAR* GetProperty(DocumentProperty prop) override;

    bool SupportsAnnotation(bool forSaving = false) const override;
    void UpdateUserAnnotations(Vec<PageAnnotation>* list) override;

    bool BenchLoadPage(int pageNo) override;

    Vec<PageElement*>* GetElements(int pageNo) override;
    PageElement* GetElementAtPos(int pageNo, PointD pt) override;

    PageDestination* GetNamedDest(const WCHAR* name) override;
    DocTocTree* GetTocTree() override;

    WCHAR* GetPageLabel(int pageNo) const override;
    int GetPageByLabel(const WCHAR* label) const override;

    bool Load(const WCHAR* fileName, PasswordUI* pwdUI);

    static EngineBase* CreateFromFile(const WCHAR* fileName, PasswordUI* pwdUI);

  protected:
    int pageCount = -1;

    DocTocTree* tocTree = nullptr;
};

EnginePdfMultiImpl::EnginePdfMultiImpl() {
    kind = kindEnginePdfMulti;
    defaultFileExt = L".vbkm";
    fileDPI = 72.0f;
}

EnginePdfMultiImpl::~EnginePdfMultiImpl() {
}
EngineBase* EnginePdfMultiImpl::Clone() {
    return nullptr;
}

int EnginePdfMultiImpl::PageCount() const {
    return pageCount;
}

RectD EnginePdfMultiImpl::PageMediabox(int pageNo) {
    return {};
}
RectD EnginePdfMultiImpl::PageContentBox(int pageNo, RenderTarget target) {
    return {};
}

RenderedBitmap* EnginePdfMultiImpl::RenderBitmap(int pageNo, float zoom, int rotation,
                                                 RectD* pageRect, /* if nullptr: defaults to the page's mediabox */
                                                 RenderTarget target, AbortCookie** cookie_out) {
    return nullptr;
}

PointD EnginePdfMultiImpl::Transform(PointD pt, int pageNo, float zoom, int rotation, bool inverse) {
    return {};
}
RectD EnginePdfMultiImpl::Transform(RectD rect, int pageNo, float zoom, int rotation, bool inverse) {
    return {};
}

std::string_view EnginePdfMultiImpl::GetFileData() {
    return {};
}

bool EnginePdfMultiImpl::SaveFileAs(const char* copyFileName, bool includeUserAnnots) {
    return false;
}

bool EnginePdfMultiImpl::SaveFileAsPdf(const char* pdfFileName, bool includeUserAnnots) {
    return false;
}

WCHAR* EnginePdfMultiImpl::ExtractPageText(int pageNo, RectI** coordsOut) {
    return nullptr;
}

bool EnginePdfMultiImpl::HasClipOptimizations(int pageNo) {
    return true;
}

WCHAR* EnginePdfMultiImpl::GetProperty(DocumentProperty prop) {
    return nullptr;
}

bool EnginePdfMultiImpl::SupportsAnnotation(bool forSaving) const {
    return false;
}

void EnginePdfMultiImpl::UpdateUserAnnotations(Vec<PageAnnotation>* list) {
}

bool EnginePdfMultiImpl::BenchLoadPage(int pageNo) {
    return false;
}

Vec<PageElement*>* EnginePdfMultiImpl::GetElements(int pageNo) {
    return nullptr;
}

PageElement* EnginePdfMultiImpl::GetElementAtPos(int pageNo, PointD pt) {
    return nullptr;
}

PageDestination* EnginePdfMultiImpl::GetNamedDest(const WCHAR* name) {
    return nullptr;
}
DocTocTree* EnginePdfMultiImpl::GetTocTree() {
    return nullptr;
}

WCHAR* EnginePdfMultiImpl::GetPageLabel(int pageNo) const {
    return nullptr;
}

int EnginePdfMultiImpl::GetPageByLabel(const WCHAR* label) const {
    return -1;
}

struct VbkmFile {
    char* path = nullptr;
    EngineBase* engine = nullptr;
};

struct ParsedVbkm {
    Vec<VbkmFile*> files;
};

// each logical record starts with "file:" line
// we split s into list of records for each file
// TODO: should we fail if the first line is not "file:" ?
// Currently we ignore everything from the beginning
// until first "file:" line
static Vec<std::string_view> SplitVbkmIntoRecords(std::string_view s) {
    Vec<std::string_view> res;
    auto tmp = s;
    Vec<const char*> addrs;

    // find indexes of lines that start with "file:"
    while (!tmp.empty()) {
        auto line = str::ParseUntil(tmp, '\n');
        if (sv::StartsWith(line, "file:")) {
            addrs.push_back(line.data());
        }
    }

    size_t n = addrs.size();
    if (n == 0) {
        return res;
    }
    addrs.push_back(s.data() + s.size());
    for (size_t i = 0; i < n; i++) {
        const char* start = addrs[i];
        const char* end = addrs[i + 1];
        size_t size = end - start;
        auto sv = std::string_view{start, size};
        res.push_back(sv);
    }
    return res;
}

std::string_view NormalizeNewlines(std::string_view s) {
    str::Str tmp(s);
    tmp.Replace("\r\n", "\n");
    tmp.Replace("\r", "\n");
    return tmp.StealAsView();
}

static std::string_view ParseLineFile(std::string_view s) {
    auto parts = sv::Split(s, ':', 2);
    if (parts.size() != 2) {
        return {};
    }
    return parts[1];
}

// parse a .vbkm record starting with "file:" line
static VbkmFile* ParseVbkmRecord(std::string_view s) {
    auto line = str::ParseUntil(s, '\n');
    auto fileName = ParseLineFile(line);
    fileName = sv::TrimSpace(fileName);
    if (fileName.empty()) {
        return nullptr;
    }
    auto res = new VbkmFile();
    res->path = str::Dup(fileName);
    // TODO: parse more stuff
    return res;
}

static ParsedVbkm* ParseVbkmFile(std::string_view d) {
    AutoFree s = NormalizeNewlines(d);
    auto records = SplitVbkmIntoRecords(s.as_view());
    auto n = records.size();
    if (n == 0) {
        return nullptr;
    }
    auto res = new ParsedVbkm();
    for (size_t i = 0; i < n; i++) {
        auto file = ParseVbkmRecord(records[i]);
        if (file == nullptr) {
            delete res;
            return nullptr;
        }
        res->files.push_back(file);
    }

    return res;
}

bool EnginePdfMultiImpl::Load(const WCHAR* fileName, PasswordUI* pwdUI) {
    auto sv = file::ReadFile(fileName);
    if (sv.empty()) {
        return false;
    }
    auto res = ParseVbkmFile(sv);
    delete res;
    return false;
}

EngineBase* EnginePdfMultiImpl::CreateFromFile(const WCHAR* fileName, PasswordUI* pwdUI) {
    if (str::IsEmpty(fileName)) {
        return nullptr;
    }
    EnginePdfMultiImpl* engine = new EnginePdfMultiImpl();
    if (!engine->Load(fileName, pwdUI)) {
        delete engine;
        return nullptr;
    }
    return engine;
}

bool IsEnginePdfMultiSupportedFile(const WCHAR* fileName, bool sniff) {
    if (sniff) {
        // we don't support sniffing
        return false;
    }
    return str::EndsWithI(fileName, L".vbkm");
}

EngineBase* CreateEnginePdfMultiFromFile(const WCHAR* fileName, PasswordUI* pwdUI) {
    return EnginePdfMultiImpl::CreateFromFile(fileName, pwdUI);
}
