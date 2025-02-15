// MIT Licensed
// see https://github.com/Paril/angelscript-ui-debugger

#pragma once

/*
 * 
 * a lightweight debugger for AngelScript. Built originally for Q2AS,
 * but hopefully usable for other purposes.
 * Design philosophy:
 * - zero overhead unless any debugging features are actually in use
 * - renders to an ImGui window
 * - only renders elements when requested; all rendered elements
 *   are cached by type + address.
 * - subclass to change how certain elements are rendered, etc.
 * - uses STL stuff to be portable.
 * - requires either fmt or std::format
 */

#include <unordered_map>
#include <unordered_set>
#include <type_traits>
#include <string>
#include <map>
#include <fmt/format.h>
#include <variant>
#include "angelscript.h"

template <class T>
inline void asIDBHashCombine(size_t &seed, const T& v)
{
    std::hash<T> hasher;
    seed ^= hasher(v) + 0x9e3779b9 + (seed<<6) + (seed>>2);
}

enum class asIDBExpandType : uint8_t
{
    None,      // no expansion
    Children,  // expands to display children
    Value      // expands to display value
};

struct asIDBTypeId
{
    int                 typeId;
    asETypeModifiers    modifiers;

    constexpr bool operator==(const asIDBTypeId &other) const
    {
        return typeId == other.typeId && modifiers == other.modifiers;
    }
};

template<>
struct std::hash<asIDBTypeId>
{
    inline std::size_t operator()(const asIDBTypeId &key) const
    {
        size_t h = std::hash<int>()(key.typeId);
        asIDBHashCombine(h, std::hash<asETypeModifiers>()(key.modifiers));
        return h;
    }
};

using asIDBTypeNameMap = std::unordered_map<asIDBTypeId, std::string>;

struct asIDBVarAddr
{
    int     typeId;
    void    *address;

    asIDBVarAddr(const asIDBVarAddr &) = default;

    constexpr bool operator==(const asIDBVarAddr &other) const
    {
        return typeId == other.typeId && address == other.address;
    }
};

template<>
struct std::hash<asIDBVarAddr>
{
    inline std::size_t operator()(const asIDBVarAddr &key) const
    {
        size_t h = std::hash<int>()(key.typeId);
        asIDBHashCombine(h, std::hash<void *>()(key.address));
        return h;
    }
};

using asIDBVarMap = std::unordered_map<asIDBVarAddr, struct asIDBVarState>;

// variables can be referenced by different names.
// this lets them retain their proper decl.
struct asIDBVarView
{
    std::string              name;
    std::string_view         type;
    asIDBVarMap::iterator    var;
    
    bool operator==(const asIDBVarView &other) const;
};

using asIDBVarViewVector = std::vector<asIDBVarView>;

// an individual value rendered out by the debugger.
struct asIDBVarValue
{
    bool disabled = false; // render with a different style
    asIDBExpandType expandable = asIDBExpandType::None;
    std::string value; // value to display in a value column or when expanded

    inline asIDBVarValue(const char *v, bool disabled = false, asIDBExpandType expandable = asIDBExpandType::None) :
        disabled(disabled),
        expandable(expandable),
        value(v ? v : "")
    {
    }

    inline asIDBVarValue(std::string v, bool disabled = false, asIDBExpandType expandable = asIDBExpandType::None) :
        disabled(disabled),
        expandable(expandable),
        value(v)
    {
    }
    
    asIDBVarValue(const asIDBVarValue &) = default;
    asIDBVarValue(asIDBVarValue &&) = default;
    asIDBVarValue &operator=(const asIDBVarValue &) = default;
    asIDBVarValue &operator=(asIDBVarValue &&) = default;
    asIDBVarValue() = default;
};

// a variable displayed in the debugger.
struct asIDBVarState
{
    asIDBVarValue value = {};
    std::unique_ptr<uint8_t[]> stackMemory; // if we're referring to a temporary value and not a handle
                                         // we have to make a copy of the value here since it won't
                                         // be available after the context is called (for getting
                                         // array elements, calling property getters, etc).

    bool queriedChildren = false;
    asIDBVarViewVector children;
};

enum class asIDBLocalType : uint8_t
{
    Parameter, // parameter sent to function
    Variable,  // local named variable
    Temporary  // a temporary; has no name but has a stack offset & type
};

// key used for storage into the local map.
struct asIDBLocalKey
{
    uint8_t          offset;
    asIDBLocalType   type;    

    inline asIDBLocalKey(int offset, asIDBLocalType type) :
        offset(offset),
        type(type)
    {
    }

    constexpr bool operator==(const asIDBLocalKey &k) const
    {
        return offset == k.offset && type == k.type;
    }
};

template<>
struct std::hash<asIDBLocalKey>
{
    inline std::size_t operator()(const asIDBLocalKey &key) const
    {
        std::size_t h = std::hash<uint8_t>()(key.offset);
        asIDBHashCombine(h, std::hash<asIDBLocalType>()(key.type));
        return h;
    }
};

using asIDBLocalMap = std::unordered_map<asIDBLocalKey, asIDBVarViewVector>;

// map of script source path -> canonical name.
using asIDBSectionSet = std::map<std::string_view, std::string_view>;

struct asIDBCallStackEntry
{
    std::string         declaration;
    std::string_view    section;
    int                 row, column;
};

using asIDBCallStackVector = std::vector<asIDBCallStackEntry>;

// this class holds the cached state of stuff
// so that we're not querying things from AS
// every frame. You should only ever make one of these
// once you have a context that you are debugging.
// It should be destroyed once that context is
// destroyed.
class asIDBCache
{
private:
    asIDBCache() = delete;
    asIDBCache(const asIDBCache &) = delete;
    asIDBCache &operator=(const asIDBCache &) = delete;

public:
    asIScriptContext *ctx;

    // cache of type id+modifiers to names
    asIDBTypeNameMap type_names;

    // cache of data for type+addr
    asIDBVarMap var_states;

    // cached globals
    bool globalsCached = false;
    asIDBVarViewVector globals;

    // cached locals
    asIDBLocalMap locals;

    // cached watch
    asIDBVarViewVector watch;
    asIDBVarViewVector::iterator removeFromWatch = watch.end(); // set to iterator we want to remove

    // cached sections
    asIDBSectionSet sections;

    // cached call stack
    std::string system_function;
    asIDBCallStackVector call_stack;

    inline asIDBCache(asIScriptContext *ctx) :
        ctx(ctx)
    {
        ctx->AddRef();
    }
    
    virtual ~asIDBCache()
    {
        ctx->ClearLineCallback();
        ctx->Release();
    }

    // caches all of the global properties in the context.
    virtual void CacheGlobals();

    // caches all of the locals with the specified key.
    virtual void CacheLocals(asIDBLocalKey stack_entry);

    // for the given var iterator, calculate the children.
    // the default implementation does the following:
    // - dereferences the handle, if it is one
    // - iterates properties on the object (or script object)
    //   and adds them to the children
    // - if the type has an `opForBegin`, it will iterate
    //   and add any opForValue's it finds.
    virtual void QueryVariableChildren(asIDBVarMap::iterator varIt);
    
    // add script sections; note that this must be done entirely
    // by an overridden class, and you'll have to keep track of
    // this data yourself, because AS doesn't currently provide
    // a way to know where all script sections used are from.
    // If this is not implemented, it simply registers all of
    // the sections it can find with functions.
    virtual void CacheSections();

    // called when the debugger has broken and it needs
    // to refresh certain cached entries.
    virtual void Refresh();
    
protected:

    // get a safe view into a cached type string.
    virtual const std::string_view GetTypeNameFromType(asIDBTypeId id);

    // adds the variable state for the given type, if it
    // doesn't already exist.
    asIDBVarMap::iterator AddVarState(asIDBVarAddr id, bool &exists)
    {
        auto v = var_states.try_emplace(id);
        exists = !v.second;
        return v.first;
    }

    // for the given var id, return a rendered var value.
    // the default implementation does the following:
    // - primitives return a formatted representation
    //   of their value.
    // - enums return a formatted int32 (todo)
    // - funcdefs return the function name
    // - uninitialized values return a disabled "uninit"
    // - any type that supports `opForBegin` will render
    //   how many elements can be iterated with it (only
    //   uint iterators are supported atm) and mark it as
    //   expandable if it has > 0 elements
    // - any type with properties will render nothing, but
    //   mark that it is expandable
    virtual asIDBVarValue FetchNodeValue(asIDBVarAddr id);

    // adds to cache.
    virtual void EnsureSectionCached(std::string_view section);

    // cache call stack entries, just for speed up when
    // rendering the UI.
    virtual void CacheCallstack();
};

struct asIDBBreakpointLocation
{
    std::string_view    section;
    int                 line;

    constexpr bool operator==(const asIDBBreakpointLocation &k) const
    {
        return section == k.section && line == k.line;
    }
};

template<>
struct std::hash<asIDBBreakpointLocation>
{
    inline std::size_t operator()(const asIDBBreakpointLocation &key) const
    {
        std::size_t h = std::hash<std::string_view>()(key.section);
        asIDBHashCombine(h, key.line);
        return h;
    }
};

struct asIDBBreakpoint
{
private:
    asIDBBreakpoint() = default;

public:
    std::variant<asIDBBreakpointLocation, std::string>  location;

    static asIDBBreakpoint Function(std::string_view f)
    {
        asIDBBreakpoint bp;
        bp.location = std::string(f);
        return bp;
    }

    static asIDBBreakpoint FileLocation(asIDBBreakpointLocation loc)
    {
        asIDBBreakpoint bp;
        bp.location = loc;
        return bp;
    }

    constexpr bool operator==(const asIDBBreakpoint &k) const
    {
        return location == k.location;
    }
};

template<>
struct std::hash<asIDBBreakpoint>
{
    inline std::size_t operator()(const asIDBBreakpoint &key) const
    {
        std::size_t h = std::hash<uint8_t>()(key.location.index() == 0 ? 0x40000000 : 0x00000000);
        if (key.location.index() == 0)
            asIDBHashCombine(h, std::get<0>(key.location));
        else
            asIDBHashCombine(h, std::get<1>(key.location));
        return h;
    }
};

enum class asIDBAction : uint8_t
{
    None,
    StepInto,
    StepOver,
    StepOut
};

// This is the main class for interfacing with
// the debugger. This manages the debugger thread
// and the 'state' of the debugger itself. The debugger
// only needs to be kept alive if it still has work to do,
// but be careful about destroying the debugger if any
// contexts are still attached to it.
/*abstract*/ class asIDBDebugger
{
public:
    // active breakpoints
    std::unordered_set<asIDBBreakpoint> breakpoints;

    asIDBAction action = asIDBAction::None;
    asUINT stack_size = 0; // for certain actions

    // cache for the current active broken state.
    // you can safely clear this cache any time the
    // debugger is not active.
    std::unique_ptr<asIDBCache> cache;

    asIDBDebugger() { }
    virtual ~asIDBDebugger() { }

    // hooks the context onto the debugger; this will
    // reset the cache, and unhook the previous context
    // from the debugger. You'll want to call this if
    // HasWork() returns true and you're requesting
    // a new context / executing code from a context
    // that isn't already hooked.
    void HookContext(asIScriptContext *ctx);

    // break on the current context. Creates the cache
    // and then suspends. Note that the cache will
    // add a reference to this context, preventing it
    // from being deleted until the cache is reset.
    void DebugBreak(asIScriptContext *ctx);

    // check if we have any work left to do.
    // it is only safe to destroy asIDBDebugger
    // if this returns false. If it returns true,
    // a context still has a linecallback set
    // using this debugger.
    bool HasWork();

    // debugger operations; these set the next breakpoint
    // and call Resume.
    void StepInto();
    void StepOver();
    void StepOut();

    // called when the debugger is being asked to resume.
    virtual void Resume() = 0;

    // breakpoint stuff
    bool ToggleBreakpoint(std::string_view section, int line);

protected:
    // called when the debugger is being asked to pause.
    // generally don't call directly, use DebugBreak.
    virtual void Suspend() = 0;

    // create a cache for the given context.
    virtual std::unique_ptr<asIDBCache> CreateCache(asIScriptContext *ctx) = 0;

    static void LineCallback(asIScriptContext *ctx, asIDBDebugger *debugger);
};
