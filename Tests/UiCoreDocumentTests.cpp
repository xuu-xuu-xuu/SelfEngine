#include "FunctionLayer/UI/Core/UiDocument.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
using namespace se::ui;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

UiElementHandle MakeChild(UiDocument& document, UiElementHandle parent, std::string_view name)
{
    const UiElementHandle child = document.CreateElement(UiElementKind::Label);
    UiDocumentError error = UiDocumentError::None;
    Check(document.SetParent(child, parent, &error), "attach child");
    Check(error == UiDocumentError::None, "attach child error");
    if (!name.empty())
    {
        Check(document.SetName(child, name, &error), "set child name");
        Check(error == UiDocumentError::None, "set child name error");
    }
    return child;
}

void Tree()
{
    UiDocument document;
    Check(document.IsValid(), "document valid");
    const UiElementHandle root = document.Root();
    Check(root.IsValid() && document.RootElement() != nullptr, "root exists");
    Check(document.ElementCount() == 1, "root is the only element");
    Check(document.Element(root)->Id().value == 1, "root stable id");

    const UiElementHandle a = MakeChild(document, root, "a");
    const UiElementHandle b = MakeChild(document, root, "b");
    const UiElementHandle c = document.CreateElement(UiElementKind::Button);
    Check(document.SetParent(c, a), "attach grandchild");
    Check(document.ChildCount(root) == 2, "root child count");
    Check(document.FirstChild(root) == a, "first child");
    Check(document.NextSibling(a) == b, "next sibling");
    Check(!document.NextSibling(b).IsValid(), "last sibling");
    Check(document.Element(a)->Parent() == root, "parent link");
    Check(document.Element(c)->Parent() == a, "grandchild parent");

    const UiId stable = document.Element(a)->Id();
    Check(document.SetParent(a, b), "reparent");
    Check(document.Element(a)->Id() == stable, "stable id survives reparent");
    Check(document.Element(a)->Handle() == a, "handle survives reparent");
    Check(document.ChildCount(root) == 1 && document.FirstChild(root) == b, "old parent updated");
    Check(document.ChildCount(b) == 1 && document.FirstChild(b) == a, "new parent updated");
    Check(document.Find(stable) == document.Element(a), "find by id after reparent");

    Check(document.SetParent(c, a), "move grandchild");
    Check(document.ChildCount(a) == 1 && document.FirstChild(a) == c, "grandchild moved");

    UiDocumentError error = UiDocumentError::None;
    Check(document.SetParent(b, a) == false, "cycle rejected");
    Check(document.SetParent(b, a, &error) == false && error == UiDocumentError::Cycle, "cycle error");
    Check(document.Element(b)->Parent() == root, "tree unchanged after cycle rejection");

    error = UiDocumentError::None;
    Check(document.SetParent(root, a, &error) == false && error == UiDocumentError::RootOperation, "root cannot move");
    Check(document.DestroyElement(root, &error) == false && error == UiDocumentError::RootOperation, "root cannot be destroyed");
    Check(document.SetParent(a, UiElementHandle{1, 0xFFFFFFFFu}, &error) == false && error == UiDocumentError::InvalidParent, "stale parent rejected");
}

void NamesAndClasses()
{
    UiDocument document;
    const UiElementHandle root = document.Root();
    const UiElementHandle a = MakeChild(document, root, "alpha");
    const UiElementHandle b = document.CreateElement(UiElementKind::Label);
    Check(document.SetParent(b, root), "attach b");

    Check(document.FindByName("alpha") == document.Element(a), "find by name");
    UiDocumentError error = UiDocumentError::None;
    Check(document.SetName(b, "alpha", &error) == false && error == UiDocumentError::DuplicateName, "duplicate name rejected");
    Check(document.Name(b).empty(), "duplicate name not applied");
    Check(document.SetName(a, "beta", &error), "rename");
    Check(error == UiDocumentError::None, "rename error");
    Check(document.FindByName("alpha") == nullptr && document.FindByName("beta") == document.Element(a), "name index updated");

    Check(document.AddClass(a, "primary"), "add class");
    Check(document.AddClass(a, "wide"), "add second class");
    Check(document.AddClass(a, "primary"), "duplicate class accepted");
    Check(document.Element(a)->Classes().size() == 2, "duplicate class deduplicated");
    Check(document.HasClass(a, "primary") && document.HasClass(a, "wide"), "has class");
    Check(!document.HasClass(a, "missing"), "missing class");
    const UiNameId first = document.Element(a)->Classes().front();
    Check(document.ClassName(first) == "primary" || document.ClassName(first) == "wide", "class name resolves");
    Check(document.RemoveClass(a, "primary"), "remove class");
    Check(!document.HasClass(a, "primary") && document.HasClass(a, "wide"), "class removed");
}

void ContentAndDirty()
{
    UiDocument document;
    const UiElementHandle root = document.Root();
    const UiElementHandle a = MakeChild(document, root, "a");
    const UiElementHandle b = MakeChild(document, root, "b");
    document.ClearAllDirty();
    Check(document.DirtyCount() == 0, "all clean");

    Check(document.SetText(a, "hello"), "set text");
    Check(document.Text(a) == "hello", "text value");
    Check(!document.SetText(a, "hello"), "same text is not a change");
    Check(HasFlag(document.Dirty(a), UiDirtyFlags::Layout) && HasFlag(document.Dirty(a), UiDirtyFlags::Paint), "text dirties layout/paint");
    Check(HasFlag(document.Dirty(root), UiDirtyFlags::Layout) && HasFlag(document.Dirty(root), UiDirtyFlags::Paint), "text dirties ancestors");
    Check(document.Dirty(b) == UiDirtyFlags::None, "sibling not dirtied");

    document.ClearAllDirty();
    Check(document.SetVisible(a, false), "set hidden");
    Check(!document.Visible(a), "hidden value");
    Check(!document.SetVisible(a, false), "same visibility is not a change");
    Check(HasFlag(document.Dirty(a), UiDirtyFlags::Layout), "visibility dirties layout");

    document.ClearAllDirty();
    Check(document.SetEnabled(a, false), "set disabled");
    Check(!document.Enabled(a), "disabled value");
    Check(!document.SetEnabled(a, false), "same enabled is not a change");
    Check(HasFlag(document.Dirty(a), UiDirtyFlags::Paint), "enabled dirties paint");
    Check(!HasFlag(document.Dirty(a), UiDirtyFlags::Layout), "enabled does not dirty layout");
    Check(HasFlag(document.Dirty(root), UiDirtyFlags::Paint), "enabled dirties ancestor paint");

    document.ClearAllDirty();
    document.MarkDirty(a, UiDirtyFlags::Layout);
    Check(HasFlag(document.Dirty(a), UiDirtyFlags::Layout), "manual layout dirty");
    Check(HasFlag(document.Dirty(root), UiDirtyFlags::Layout) && HasFlag(document.Dirty(root), UiDirtyFlags::Paint), "manual layout propagates");
    Check(document.DirtyCount() == 2, "manual layout dirty count");

    document.ClearAllDirty();
    document.MarkDirty(a, UiDirtyFlags::Structure);
    Check(HasFlag(document.Dirty(a), UiDirtyFlags::Structure), "manual structure dirty");
    Check(HasFlag(document.Dirty(root), UiDirtyFlags::Structure), "structure propagates");
    Check(HasFlag(document.Dirty(a), UiDirtyFlags::Layout) && HasFlag(document.Dirty(a), UiDirtyFlags::Paint), "structure implies layout/paint");

    document.ClearAllDirty();
    document.MarkDirty(a, UiDirtyFlags::Paint);
    Check(HasFlag(document.Dirty(root), UiDirtyFlags::Paint), "paint propagates");
    Check(!HasFlag(document.Dirty(root), UiDirtyFlags::Layout), "paint does not imply layout");

    document.ClearDirty(a, UiDirtyFlags::All);
    Check(document.Dirty(a) == UiDirtyFlags::None, "clear all flags on node");

    document.ClearAllDirty();
    Check(document.AddClass(a, "dirty-class"), "class change for dirty test");
    Check(HasFlag(document.Dirty(a), UiDirtyFlags::Style), "class change dirties style");
    Check(HasFlag(document.Dirty(a), UiDirtyFlags::Layout), "class change dirties layout");
    Check(HasFlag(document.Dirty(root), UiDirtyFlags::Layout), "class change propagates layout");
    Check(!HasFlag(document.Dirty(root), UiDirtyFlags::Style), "style does not propagate as style");
}

void Destruction()
{
    UiDocument document;
    const UiElementHandle root = document.Root();
    const UiElementHandle parent = MakeChild(document, root, "parent");
    const UiElementHandle child = MakeChild(document, parent, "child");
    const UiElementHandle grandchild = MakeChild(document, child, "grandchild");
    const UiId parentId = document.Element(parent)->Id();
    Check(document.ElementCount() == 4, "subtree count");

    Check(document.DestroyElement(parent), "destroy subtree");
    Check(document.Element(parent) == nullptr, "parent destroyed");
    Check(document.Element(child) == nullptr, "child destroyed");
    Check(document.Element(grandchild) == nullptr, "grandchild destroyed");
    Check(document.Find(parentId) == nullptr, "stable id removed");
    Check(document.ChildCount(root) == 0, "parent detached");
    Check(document.ElementCount() == 1, "only root remains");
    Check(document.FindByName("child") == nullptr, "name index removed");

    const UiElementHandle leaf = MakeChild(document, root, "generation");
    const UiElementHandle oldHandle = leaf;
    const UiId oldId = document.Element(leaf)->Id();
    const std::uint32_t index = leaf.index;
    Check(document.DestroyElement(leaf), "destroy leaf");
    const UiElementHandle replacement = document.CreateElement(UiElementKind::Label);
    Check(replacement.index == index, "free slot reused");
    Check(replacement.generation != oldHandle.generation, "generation advanced");
    Check(document.Element(oldHandle) == nullptr, "old generation handle rejected");
    Check(document.Element(replacement) != nullptr, "new handle valid");
    Check(document.Element(replacement)->Id() != oldId, "stable id is not reused");

    UiDocumentError error = UiDocumentError::None;
    Check(document.DestroyElement(UiElementHandle{}, &error) == false && error == UiDocumentError::InvalidHandle, "invalid destroy rejected");

    Check(document.SetParent(replacement, root), "attach for detach test");
    Check(document.SetParent(replacement, UiElementHandle{}), "detach with invalid parent handle");
    Check(!document.Element(replacement)->Parent().IsValid(), "detached parent cleared");
    Check(document.ChildCount(root) == 0, "detached from root");
    Check(document.SetParent(replacement, root), "reattach");
    Check(document.ChildCount(root) == 1, "reattach count");
}

void MoveAndKinds()
{
    UiDocument document;
    const UiElementHandle root = document.Root();
    Check(document.SetKind(root, UiElementKind::Panel), "set root kind");
    const UiElementHandle label = document.CreateElement(UiElementKind::Panel);
    Check(document.SetParent(label, root), "attach label");
    Check(document.SetKind(label, UiElementKind::Label), "change kind");
    Check(document.Element(label)->Kind() == UiElementKind::Label, "kind value");
    Check(!document.SetKind(UiElementHandle{}, UiElementKind::Button), "invalid kind rejected");

    UiDocument moved(std::move(document));
    Check(moved.IsValid() && !document.IsValid(), "document move");
    Check(moved.Element(label) != nullptr, "handle belongs to the moved document");
    Check(moved.Element(moved.Root()) != nullptr, "moved document root");
    Check(moved.IsValid() && moved.ElementCount() == 2, "moved document count");

    document = std::move(moved);
    Check(document.IsValid() && moved.IsValid() == false, "move assignment");
    Check(document.ElementCount() == 2, "move assignment count");
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        Tree();
        NamesAndClasses();
        ContentAndDirty();
        Destruction();
        MoveAndKinds();

        if (argc > 1)
        {
            const std::filesystem::path proof = argv[1];
            if (!proof.parent_path().empty())
            {
                std::filesystem::create_directories(proof.parent_path());
            }
            std::ofstream(proof)
                << "{\"success\":true,\"tree\":true,\"stableId\":true,\"cycleRejected\":true,"
                   "\"generation\":true,\"dirtyPropagation\":true,\"classes\":true}\n";
        }
        std::cout << "UiCoreDocumentTests passed (arena, stable handles, tree, names/classes, dirty flags).\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
