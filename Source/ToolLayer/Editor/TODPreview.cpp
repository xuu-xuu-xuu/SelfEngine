#include "TODPreview.h"
namespace se
{
bool TODPreview::Open(AssetStore& assets,const AssetId& id)
{
    std::vector<AssetDiagnostic> diagnostics;const auto candidate=assets.LoadTODProfile(id,diagnostics);
    if(!candidate){error_=diagnostics.empty()?"TOD.ReadFailed":diagnostics.back().key;return false;}
    std::string encoded;if(!SerializeTODProfile(*candidate,encoded,error_))return false;
    if(!environment_.TimeOfDay().SelectProfile(*candidate)){error_=environment_.TimeOfDay().LastError();return false;}
    profile_=*candidate;id_=id;path_=assets.Resolve(id);saved_=std::move(encoded);environment_.SetPaused(true);error_.clear();return true;
}
bool TODPreview::Apply(const TODProfile& candidate)
{
    if(!Ready()){error_="TOD.NoProfile";return false;}
    if(!environment_.TimeOfDay().SelectProfile(candidate,true)){error_=environment_.TimeOfDay().LastError();return false;}
    profile_=candidate;error_.clear();return true;
}
bool TODPreview::Save()
{
    if(!Ready()){error_="TOD.NoProfile";return false;}
    std::string encoded;if(!SerializeTODProfile(profile_,encoded,error_)||!SaveTODProfile(path_,profile_,error_))return false;saved_=std::move(encoded);return true;
}
bool TODPreview::Dirty()const{std::string text,error;return Ready()&&SerializeTODProfile(profile_,text,error)&&text!=saved_;}
}
