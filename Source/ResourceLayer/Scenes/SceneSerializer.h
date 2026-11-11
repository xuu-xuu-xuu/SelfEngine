#pragma once

#include "FunctionLayer/Runtime/World.h"
#include <filesystem>
#include <string>
#include <string_view>
#include <memory>

namespace se
{
// 文件线程只生成不可变文档；实例化/组件工厂与生命周期留在主线程。
class SceneDocument
{
public:
    explicit operator bool()const noexcept{return static_cast<bool>(root_);}
private:
    struct Data;
    std::shared_ptr<const Data> root_;
    friend bool ParseSceneDocument(std::string_view,SceneDocument&,std::string&);
    friend bool InstantiateScene(const SceneDocument&,World&,std::string&);
};
bool ParseSceneDocument(std::string_view text,SceneDocument& document,std::string& error);
bool ReadSceneDocument(const std::filesystem::path& path,SceneDocument& document,std::string& error);
bool InstantiateScene(const SceneDocument& document,World& world,std::string& error);
// 失败时 world 与已有文件均保持原状态；error 返回稳定文本键。
bool SerializeScene(const World& world, std::string& jsonText, std::string& error);
bool DeserializeScene(std::string_view jsonText, World& world, std::string& error);
bool SaveScene(const std::filesystem::path& path,const World& world,std::string& error,bool replaceExisting=true);
bool LoadScene(const std::filesystem::path& path, World& world, std::string& error);
}
