#include "MaterialEditSession.h"

namespace se
{
namespace
{
std::string Encode(const MaterialDocument& document)
{std::string text;std::vector<AssetDiagnostic> diagnostics;SerializeMaterialDocument(document,text,diagnostics);return text;}
}
bool MaterialEditSession::Reset(const MaterialDocument& document,bool saved)
{
    if(!ValidateMaterialDocument(document))return Fail("Assets.InvalidMaterial");
    document_=document;saved_=saved?Encode(document):std::string{};undo_.clear();redo_.clear();transaction_.reset();error_.clear();return true;
}
void MaterialEditSession::Push(MaterialDocument before,MaterialDocument after)
{
    if(Encode(before)==Encode(after))return;
    if(undo_.size()==100)undo_.erase(undo_.begin());
    undo_.push_back({std::move(before),std::move(after)});redo_.clear();
}
bool MaterialEditSession::SetProperty(std::string_view key,const PropertyValue& value)
{
    auto before=document_;if(!WriteMaterialProperty(document_,key,value,error_))return false;
    if(!transaction_)Push(std::move(before),document_);error_.clear();return true;
}
bool MaterialEditSession::BeginEdit()
{if(transaction_)return Fail("Editor.TransactionActive");transaction_=document_;error_.clear();return true;}
bool MaterialEditSession::CommitEdit()
{if(!transaction_)return Fail("Editor.NoTransaction");auto before=std::move(*transaction_);transaction_.reset();Push(std::move(before),document_);error_.clear();return true;}
bool MaterialEditSession::CancelEdit()
{if(!transaction_)return Fail("Editor.NoTransaction");document_=std::move(*transaction_);transaction_.reset();error_.clear();return true;}
bool MaterialEditSession::Undo()
{if(transaction_||undo_.empty())return Fail("Editor.CannotUndo");document_=undo_.back().before;redo_.push_back(std::move(undo_.back()));undo_.pop_back();error_.clear();return true;}
bool MaterialEditSession::Redo()
{if(transaction_||redo_.empty())return Fail("Editor.CannotRedo");document_=redo_.back().after;undo_.push_back(std::move(redo_.back()));redo_.pop_back();error_.clear();return true;}
bool MaterialEditSession::MarkSaved()
{if(transaction_)return Fail("Editor.TransactionActive");saved_=Encode(document_);error_.clear();return true;}
bool MaterialEditSession::Dirty() const{return Encode(document_)!=saved_;}
}
