#pragma once
#include "ResourceLayer/Assets/MaterialMetadata.h"
#include <optional>

namespace se
{
// 只拥有可变文档/有限历史；文件、AssetStore 与 GPU 寿命仍由原有模块负责。
class MaterialEditSession
{
public:
    const MaterialDocument& Document() const noexcept{return document_;}
    bool Reset(const MaterialDocument& document,bool saved);
    bool SetProperty(std::string_view key,const PropertyValue& value);
    bool BeginEdit();
    bool CommitEdit();
    bool CancelEdit();
    bool Undo();
    bool Redo();
    bool MarkSaved();
    bool Dirty() const;
    bool Editing() const noexcept{return transaction_.has_value();}
    size_t UndoCount() const noexcept{return undo_.size();}
    size_t RedoCount() const noexcept{return redo_.size();}
    const std::string& LastError() const noexcept{return error_;}
private:
    struct Command{MaterialDocument before,after;};
    bool Fail(const char* key){error_=key;return false;}
    void Push(MaterialDocument before,MaterialDocument after);
    MaterialDocument document_;
    std::string saved_,error_;
    std::optional<MaterialDocument> transaction_;
    std::vector<Command> undo_,redo_;
};
}
