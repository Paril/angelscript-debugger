// MIT Licensed
// see https://github.com/Paril/angelscript-ui-debugger

#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#include <angelscript.h>
#include "as_debugger.h"
#include <bitset>

/*virtual*/ const asIDBVarAddr &asIDBVarView::GetID() /*override*/
{
    return var->first;
}

/*virtual*/ asIDBVarState &asIDBVarView::GetState() /*override*/
{
    return var->second;
}

/*virtual*/ void asIDBCache::Refresh()
{
}

/*virtual*/ const std::string_view asIDBCache::GetTypeNameFromType(asIDBTypeId id)
{
    if (auto f = type_names.find(id); f != type_names.end())
        return f->second.c_str();

    auto type = ctx->GetEngine()->GetTypeInfoById(id.typeId);
    const char *rawName = "???";

    if (!type)
    {
        // a primitive
        switch (id.typeId & asTYPEID_MASK_SEQNBR)
        {
        case asTYPEID_BOOL: rawName = "bool"; break;
        case asTYPEID_INT8: rawName = "int8"; break;
        case asTYPEID_INT16: rawName = "int16"; break;
        case asTYPEID_INT32: rawName = "int32"; break;
        case asTYPEID_INT64: rawName = "int64"; break;
        case asTYPEID_UINT8: rawName = "uint8"; break;
        case asTYPEID_UINT16: rawName = "uint16"; break;
        case asTYPEID_UINT32: rawName = "uint32"; break;
        case asTYPEID_UINT64: rawName = "uint64"; break;
        case asTYPEID_FLOAT: rawName = "float"; break;
        case asTYPEID_DOUBLE: rawName = "double"; break;
        default: rawName = "???"; break;
        }
    }
    else
    {
        rawName = type->GetName();
    }

    std::string name = fmt::format("{}{}{}", (id.modifiers & asTM_CONST) ? "const " : "", rawName,
        ((id.modifiers & asTM_INOUTREF) == asTM_INOUTREF) ? "&" :
        ((id.modifiers & asTM_INOUTREF) == asTM_INREF) ? "&in" :
        ((id.modifiers & asTM_INOUTREF) == asTM_OUTREF) ? "&out" :
        "");

    return type_names.emplace(id, std::move(name)).first->second;
}

void *asIDBCache::ResolvePropertyAddress(const asIDBResolvedVarAddr &id, int propertyIndex, int offset, int compositeOffset, bool isCompositeIndirect)
{
    if (id.source.typeId & asTYPEID_SCRIPTOBJECT)
    {
        asIScriptObject *obj = (asIScriptObject *) id.resolved;
        return obj->GetAddressOfProperty(propertyIndex);
    }

    // indirect changes our ptr to
    // *(object + compositeOffset) + offset
    if (isCompositeIndirect)
    {
        void *propAddr = *reinterpret_cast<uint8_t **>(reinterpret_cast<uint8_t *>(id.resolved) + compositeOffset);

        // if we're null, leave it alone, otherwise point to
        // where we really need to be pointing
        if (propAddr)
            propAddr = reinterpret_cast<uint8_t *>(propAddr) + offset;

        return propAddr;
    }

    return reinterpret_cast<uint8_t *>(id.resolved) + offset + compositeOffset;
}

#include <charconv>

/*virtual*/ std::optional<asIDBExprResult> asIDBCache::ResolveExpression(const std::string_view expr, int stack_index)
{
    // isolate the variable name first
    std::string_view variable_name = expr.substr(0, expr.find_first_of(".[", 0));
    asIDBVarAddr variable_key;

    // if it starts with a & it has to be a local variable index
    if (variable_name[0] == '&')
    {
        uint16_t offset;
        auto result = std::from_chars(&variable_name.front(), &variable_name.front() + variable_name.size(), offset);

        if (result.ec != std::errc())
            return std::nullopt;

        // check bounds
        int m = ctx->GetVarCount(stack_index);

        if (offset >= m)
            return std::nullopt;

        if (!ctx->IsVarInScope(offset, stack_index))
            return std::nullopt;

        // grab key
        asETypeModifiers modifiers;
        ctx->GetVar(offset, stack_index, 0, &variable_key.typeId, &modifiers);
        variable_key.constant = (modifiers & asTM_CONST) != 0;
        variable_key.address = ctx->GetAddressOfVar(offset, stack_index);
    }
    // check this
    else if (variable_name == "this")
    {
        if (!(variable_key.address = ctx->GetThisPointer(stack_index)))
            return std::nullopt;

        variable_key.typeId = ctx->GetThisTypeId(stack_index);
    }
    else
    {
        // not an offset; in order, check the following:
        // - local variables (in reverse order)
        // - function parameters
        // - class member properties (if appropriate)
        // - globals
        for (int i = ctx->GetVarCount(stack_index) - 1; i >= 0; i--)
        {
            if (!ctx->IsVarInScope(i, stack_index))
                continue;

            const char *name;
            int typeId;
            ctx->GetVar(i, stack_index, &name, &typeId);

            if (variable_name != name)
                continue;

            variable_key.typeId = typeId;
            variable_key.address = ctx->GetAddressOfVar(i, stack_index);
            break;
        }

        // check `this` parameters
        if (!variable_key.typeId)
        {
            if (auto thisPtr = ctx->GetThisPointer(stack_index))
            {
                auto thisTypeId = ctx->GetThisTypeId(stack_index);
                auto type = ctx->GetEngine()->GetTypeInfoById(thisTypeId);

                for (asUINT i = 0; i < type->GetPropertyCount(); i++)
                {
                    const char *name;
                    int typeId;
                    int offset;
                    int compositeOffset;
                    bool isCompositeIndirect;
                    bool isReadOnly;

                    type->GetProperty(i, &name, &typeId, 0, 0, &offset, 0, 0, &compositeOffset, &isCompositeIndirect, &isReadOnly);

                    if (variable_name != name)
                        continue;

                    variable_key.typeId = typeId;
                    variable_key.constant = isReadOnly;
                    variable_key.address = ResolvePropertyAddress(asIDBVarAddr { thisTypeId, false, thisPtr }, i, offset, compositeOffset, isCompositeIndirect);
                    break;
                }
            }
        }

        // check globals
        if (!variable_key.typeId)
        {
            auto main = ctx->GetFunction(0)->GetModule();

            for (asUINT n = 0; n < main->GetGlobalVarCount(); n++)
            {
                const char *name;
                int typeId;
                bool isConst;

                main->GetGlobalVar(n, &name, nullptr, &typeId, &isConst);

                if (variable_name != name)
                    continue;

                variable_key.typeId = typeId;
                variable_key.address = main->GetAddressOfGlobalVar(n);
                break;
            }
        }

        if (!variable_key.typeId)
            return std::nullopt;
    }

    // variable_key should be non-null and with
    // a valid type ID here.
    return ResolveSubExpression(variable_key, expr.substr(variable_name.size()), stack_index);
}

/*virtual*/ std::optional<asIDBExprResult> asIDBCache::ResolveSubExpression(const asIDBResolvedVarAddr &idKey, const std::string_view rest, int stack_index)
{
    // nothing left, so this is the result.
    if (rest.empty())
        return asIDBExprResult { idKey.source, evaluators.Evaluate(*this, idKey) };

    // make sure we're a type that supports properties
    auto type = ctx->GetEngine()->GetTypeInfoById(idKey.source.typeId);

    if (!type)
        return std::nullopt;
    else if (type->GetFlags() & (asOBJ_ENUM | asOBJ_FUNCDEF))
        return std::nullopt;
    // uninitialized, etc
    else if (!idKey.resolved)
        return std::nullopt;

    // check what kind of sub-evaluator to use
    std::string_view eval_name = rest.substr(0, rest.find_first_of(".[", 1));

    if (eval_name[0] == '.')
    {
        std::string_view prop_name = eval_name.substr(1);

        for (asUINT i = 0; i < type->GetPropertyCount(); i++)
        {
            const char *name;
            int typeId;
            int offset;
            int compositeOffset;
            bool isCompositeIndirect;
            bool isReadOnly;

            type->GetProperty(i, &name, &typeId, 0, 0, &offset, 0, 0, &compositeOffset, &isCompositeIndirect, &isReadOnly);

            if (prop_name != name)
                continue;

            void *propAddr = ResolvePropertyAddress(idKey, i, offset, compositeOffset, isCompositeIndirect);

            return ResolveSubExpression(asIDBVarAddr { typeId, isReadOnly, propAddr }, rest.substr(eval_name.size()), stack_index);
        }
    }
    else if (eval_name[0] == '[')
    {
        // TODO
        return std::nullopt;
    }

    return std::nullopt;
}

/*virtual*/ void asIDBCache::CacheCallstack()
{
    if (!ctx)
        return;

    call_stack.clear();

    if (auto sysfunc = ctx->GetSystemFunction())
        system_function = fmt::format("{} (system function)", sysfunc->GetDeclaration(true, false, true));
    else
        system_function = "";

    for (asUINT n = 0; n < ctx->GetCallstackSize(); n++)
    {
        asIScriptFunction *func = nullptr;
        int column = 0;
        const char *section = "";
        int row = 0;

        if (n == 0 && ctx->GetState() == asEXECUTION_EXCEPTION)
        {
            func = ctx->GetExceptionFunction();
            if (func)
                row = ctx->GetExceptionLineNumber(&column, &section);
        }
        else
        {
            func = ctx->GetFunction(n);

            if (func)
                row = ctx->GetLineNumber(n, &column, &section);
        }

        std::string decl;
        
        if (func)
            decl = fmt::format("{} Line {}", func->GetDeclaration(true, false, true), row);
        else
            decl = "???";

        call_stack.push_back(asIDBCallStackEntry {
            std::move(decl),
            section,
            row,
            column
        });

        dbg->EnsureSectionCached(call_stack.back().section, call_stack.back().section);
    }
}

// restore data from the given cache that is
// being replaced by this one.
/*virtual*/ void asIDBCache::Restore(asIDBCache &cache)
{
    watch = std::move(cache.watch);

    for (auto &w : watch)
        w.dirty = true;
}

/*virtual*/ void asIDBCache::CacheGlobals()
{
    if (!ctx)
        return;

    auto main = ctx->GetFunction(0)->GetModule();

    for (asUINT n = 0; n < main->GetGlobalVarCount(); n++)
    {
        const char *name;
        const char *nameSpace;
        int typeId;
        void *ptr;
        bool isConst;

        main->GetGlobalVar(n, &name, &nameSpace, &typeId, &isConst);
        ptr = main->GetAddressOfGlobalVar(n);

        asIDBTypeId typeKey { typeId, isConst ? asTM_CONST : asTM_NONE };
        const std::string_view viewType = GetTypeNameFromType(typeKey);

        asIDBVarAddr idKey { typeId, isConst, ptr };

        // globals can safely appear in more than one spot
        bool exists;
        auto stateIt = AddVarState(idKey, exists);

        if (!exists)
        {
            auto &state = stateIt->second;
            state.value = evaluators.Evaluate(*this, idKey);
        }

        globals.push_back(asIDBVarView { (nameSpace && nameSpace[0]) ? fmt::format("{}::{}", nameSpace, name) : name, viewType, stateIt });
    }

    globalsCached = true;
}

/*virtual*/ void asIDBCache::CacheLocals(asIDBLocalKey stack_entry)
{
    if (!ctx)
        return;

    // variables in AS are always ordered the same way it seems:
    // function parameters come first,
    // then local variables,
    // then temporaries used during calculations.
    int numParams = ctx->GetFunction(stack_entry.offset)->GetParamCount();
    int numLocals = ctx->GetVarCount(stack_entry.offset);

    int start = 0, end = 0;

    if (stack_entry.type == asIDBLocalType::Parameter)
        end = numParams;
    else
    {
        start = numParams;
        end = numLocals;
    }

    auto &map = locals[stack_entry];

    if (stack_entry.type == asIDBLocalType::Variable)
    {
        if (auto thisPtr = ctx->GetThisPointer(stack_entry.offset))
        {
            int thisTypeId = ctx->GetThisTypeId(stack_entry.offset);

            asIDBTypeId typeKey { thisTypeId, asTM_NONE };

            const std::string_view viewType = GetTypeNameFromType(typeKey);

            asIDBVarAddr idKey { thisTypeId, false, thisPtr };

            // locals can safely appear in more than one spot
            bool exists;
            auto stateIt = AddVarState(idKey, exists);

            if (!exists)
            {
                auto &state = stateIt->second;
                state.value = evaluators.Evaluate(*this, idKey);
            }

            map.push_back(asIDBVarView { "this", viewType, stateIt });
        }
    }

    for (int n = start; n < end; n++)
    {
        const char *name;
        int typeId;
        asETypeModifiers modifiers;
        int stackOffset;
        ctx->GetVar(n, stack_entry.offset, &name, &typeId, &modifiers, 0, &stackOffset);

        bool isTemporary = stack_entry.type != asIDBLocalType::Parameter && (!name || !*name);
        
        if (isTemporary && (stack_entry.type != asIDBLocalType::Temporary))
            continue;
        else if (!isTemporary && (stack_entry.type == asIDBLocalType::Temporary))
            continue;

        void *ptr = ctx->GetAddressOfVar(n, stack_entry.offset);

        asIDBTypeId typeKey { typeId, modifiers };

        std::string localName = (name && *name) ? fmt::format("{} (&{})", name, n) : fmt::format("&{}", n);

        const std::string_view viewType = GetTypeNameFromType(typeKey);

        asIDBVarAddr idKey { typeId, (modifiers & asTM_CONST) != 0, ptr };

        // locals can safely appear in more than one spot
        bool exists;
        auto stateIt = AddVarState(idKey, exists);

        if (!exists)
        {
            auto &state = stateIt->second;
            state.value = evaluators.Evaluate(*this, idKey);
        }

        map.push_back(asIDBVarView { std::move(localName), viewType, stateIt });
    }
}

class asIDBNullTypeEvaluator : public asIDBTypeEvaluator
{
public:
    virtual asIDBVarValue Evaluate(asIDBCache &, const asIDBResolvedVarAddr &id) const override { return { "(null)", true }; }
};

class asIDBUninitTypeEvaluator : public asIDBTypeEvaluator
{
public:
    virtual asIDBVarValue Evaluate(asIDBCache &, const asIDBResolvedVarAddr &id) const override { return { "(uninit)", true }; }
};

#include <array>

class asIDBEnumTypeEvaluator : public asIDBTypeEvaluator
{
public:
    virtual asIDBVarValue Evaluate(asIDBCache &cache, const asIDBResolvedVarAddr &id) const override
    {
        // for enums where we have a single matched value
        // just display it directly; it might be a mask but that's OK.
        auto type = cache.ctx->GetEngine()->GetTypeInfoById(id.source.typeId);

        union {
            asINT64 v = 0;
            asQWORD uv;
        };
        
        switch (type->GetTypedefTypeId())
        {
        case asTYPEID_INT8:
            v = *reinterpret_cast<const int8_t *>(id.resolved);
            break;
        case asTYPEID_UINT8:
            uv = *reinterpret_cast<const uint8_t *>(id.resolved);
            break;
        case asTYPEID_INT16:
            v = *reinterpret_cast<const int16_t *>(id.resolved);
            break;
        case asTYPEID_UINT16:
            uv = *reinterpret_cast<const uint16_t *>(id.resolved);
            break;
        case asTYPEID_INT32:
            v = *reinterpret_cast<const int32_t *>(id.resolved);
            break;
        case asTYPEID_UINT32:
            uv = *reinterpret_cast<const uint32_t *>(id.resolved);
            break;
        case asTYPEID_INT64:
            v = *reinterpret_cast<const int64_t *>(id.resolved);
            break;
        case asTYPEID_UINT64:
            uv = *reinterpret_cast<const uint64_t *>(id.resolved);
            break;
        }

        for (asUINT e = 0; e < type->GetEnumValueCount(); e++)
        {
            asINT64 ov = 0;
            const char *name = type->GetEnumValueByIndex(e, &ov);

            if (ov == v)
            {
                if (type->GetTypedefTypeId() >= asTYPEID_UINT8 && type->GetTypedefTypeId() <= asTYPEID_UINT64)
                    return fmt::format("{} ({})", name, uv);

                return fmt::format("{} ({})", name, v);
            }
        }
        
        std::bitset<32> bits(v);

        if (bits.count() == 1)
        {
            if (type->GetTypedefTypeId() >= asTYPEID_UINT8 && type->GetTypedefTypeId() <= asTYPEID_UINT64)
                return fmt::format("{}", uv );
            return fmt::format("{}", v );
        }

        return { fmt::format("{} bits", bits.count()), false, asIDBExpandType::Entries };
    }

    virtual void Expand(asIDBCache &cache, const asIDBResolvedVarAddr &id, asIDBVarState &state) const override
    {
        auto type = cache.ctx->GetEngine()->GetTypeInfoById(id.source.typeId);

        union {
            asINT64 v = 0;
            asQWORD uv;
        };
        
        switch (type->GetTypedefTypeId())
        {
        case asTYPEID_INT8:
            v = *reinterpret_cast<const int8_t *>(id.resolved);
            break;
        case asTYPEID_UINT8:
            uv = *reinterpret_cast<const uint8_t *>(id.resolved);
            break;
        case asTYPEID_INT16:
            v = *reinterpret_cast<const int16_t *>(id.resolved);
            break;
        case asTYPEID_UINT16:
            uv = *reinterpret_cast<const uint16_t *>(id.resolved);
            break;
        case asTYPEID_INT32:
            v = *reinterpret_cast<const int32_t *>(id.resolved);
            break;
        case asTYPEID_UINT32:
            uv = *reinterpret_cast<const uint32_t *>(id.resolved);
            break;
        case asTYPEID_INT64:
            v = *reinterpret_cast<const int64_t *>(id.resolved);
            break;
        case asTYPEID_UINT64:
            uv = *reinterpret_cast<const uint64_t *>(id.resolved);
            break;
        }
        
        if (type->GetTypedefTypeId() >= asTYPEID_UINT8 && type->GetTypedefTypeId() <= asTYPEID_UINT64)
            state.entries.push_back({ fmt::format("value: {}", uv) });
        else
            state.entries.push_back({ fmt::format("value: {}", v) });
        
        // find bit names
        asINT64 ov = 0;
        std::array<const char *, sizeof(ov) * 8> bit_names { };

        for (asUINT e = 0; e < type->GetEnumValueCount(); e++)
        {
            const char *name = type->GetEnumValueByIndex(e, &ov);
            std::bitset<sizeof(ov) * 8> obits(ov);

            // skip masks
            if (obits.count() != 1)
                continue;

            if (ov & v)
            {
                int p = 0;

                while (ov && !(ov & 1))
                {
                    ov >>= 1;
                    p++;
                }

                // only take the first name, just incase
                // there's later overrides
                if (p <= (obits.size() - 1) && !bit_names[p])
                    bit_names[p] = name;
            }
        }

        // display bits
        for (asQWORD e = 0; e < bit_names.size(); e++)
        {
            if (v & (1ull << e))
            {
                if (bit_names[e])
                    state.entries.push_back({ fmt::format("[{:2}] {}", e, bit_names[e]) });
                else
                    state.entries.push_back({ fmt::format("[{:2}] {}", e, 1 << e) });
            }
        }
    }
};

class asIDBFuncDefTypeEvaluator : public asIDBTypeEvaluator
{
public:
    virtual asIDBVarValue Evaluate(asIDBCache &, const asIDBResolvedVarAddr &id) const override
    {
        asIScriptFunction *ptr = reinterpret_cast<asIScriptFunction *>(id.resolved);
        return { ptr->GetName(), false };
    }
};

/*virtual*/ asIDBVarValue asIDBObjectTypeEvaluator::Evaluate(asIDBCache &cache, const asIDBResolvedVarAddr &id) const /*override*/
{
    auto ctx = cache.ctx;
    auto type = ctx->GetEngine()->GetTypeInfoById(id.source.typeId);
    bool canExpand = type->GetPropertyCount();
    asIDBVarValue val;

    if (ctx->GetState() != asEXECUTION_EXCEPTION)
    {
        if (auto opForBegin = type->GetMethodByName("opForBegin"))
        {
            if (opForBegin->GetReturnTypeId() != asTYPEID_UINT32)
            {
                return { "(unsup. iterator)", true, canExpand ? asIDBExpandType::Children : asIDBExpandType::None };
            }
            
            auto opForEnd = type->GetMethodByName("opForEnd");
            auto opForNext = type->GetMethodByName("opForNext");

            // if we haven't got anything special yet, and we're
            // iterable, we'll show how many elements we have.
            // we'll also just assume the code isn't busted.
            int numElements = 0;
            
            cache.dbg->internal_execution = true;
            ctx->PushState();
            ctx->Prepare(opForBegin);
            ctx->SetObject(id.resolved);
            ctx->Execute();

            uint32_t rtn = ctx->GetReturnDWord();

            while (true)
            {
                ctx->Prepare(opForEnd);
                ctx->SetObject(id.resolved);
                ctx->SetArgDWord(0, rtn);
                ctx->Execute();
                bool finished = ctx->GetReturnByte();

                if (finished)
                    break;
                
                ctx->Prepare(opForNext);
                ctx->SetObject(id.resolved);
                ctx->SetArgDWord(0, rtn);
                ctx->Execute();

                rtn = ctx->GetReturnDWord();

                numElements++;
            }

            ctx->PopState();
            cache.dbg->internal_execution = false;

            val.value = fmt::format("{} elements", numElements);
            val.disabled = true;

            if (numElements)
                canExpand = true;
        }
    }

    val.expandable = canExpand ? asIDBExpandType::Children : asIDBExpandType::None;

    return val;
}

/*virtual*/ void asIDBObjectTypeEvaluator::Expand(asIDBCache &cache, const asIDBResolvedVarAddr &id, asIDBVarState &state) const /*override*/
{
    QueryVariableProperties(cache, id, state);

    QueryVariableForEach(cache, id, state);
}

// convenience function that queries the properties of the given
// address (and object, if set) of the given type.
void asIDBObjectTypeEvaluator::QueryVariableProperties(asIDBCache &cache, const asIDBResolvedVarAddr &id, asIDBVarState &var) const
{
    auto type = cache.ctx->GetEngine()->GetTypeInfoById(id.source.typeId);

    asIScriptObject *obj = nullptr;

    if (id.source.typeId & asTYPEID_SCRIPTOBJECT)
        obj = (asIScriptObject *) id.resolved;

    for (asUINT n = 0; n < (obj ? obj->GetPropertyCount() : type->GetPropertyCount()); n++)
    {
        const char *name;
        int propTypeId;
        void *propAddr = nullptr;
        int offset;
        int compositeOffset;
        bool isCompositeIndirect;
        bool isReadOnly;

        type->GetProperty(n, &name, &propTypeId, 0, 0, &offset, 0, 0, &compositeOffset, &isCompositeIndirect, &isReadOnly);

        propAddr = cache.ResolvePropertyAddress(id, n, offset, compositeOffset, isCompositeIndirect);

        asIDBVarAddr propId { propTypeId, isReadOnly, propAddr };

        // TODO: variables that overlap memory space will
        // get culled by this. this helps in the case of
        // vec3_t::x and vec3_t::pitch for instance, but
        // causes some confusion for edict_t::number and
        // edict_t::s::number, where `s` is now just an empty
        // struct. it'd be ideal if, in this case, it prefers
        // the deeper nested ones. not sure how we'd express that
        // with the limited context we have, though.
        bool exists;
        auto state = cache.AddVarState(propId, exists);

        if (exists)
            continue;

        auto &propVar = state->second;

        propVar.value = cache.evaluators.Evaluate(cache, propId);

        var.children.push_back(asIDBVarView { name, cache.GetTypeNameFromType({ propTypeId, isReadOnly ? asTM_CONST : asTM_NONE }), state });
    }
}
    
// convenience function that iterates the opFor* of the given
// address (and object, if set) of the given type. If non-zero,
// a specific index will be used.
void asIDBObjectTypeEvaluator::QueryVariableForEach(asIDBCache &cache, const asIDBResolvedVarAddr &id, asIDBVarState &var, int index) const
{
    auto ctx = cache.ctx;

    if (ctx->GetState() == asEXECUTION_EXCEPTION)
        return;

    auto type = ctx->GetEngine()->GetTypeInfoById(id.source.typeId);

    auto opForBegin = type->GetMethodByName("opForBegin");

    if (!opForBegin || opForBegin->GetReturnTypeId() != asTYPEID_UINT32)
        return;

    auto opForEnd = type->GetMethodByName("opForEnd");
    auto opForNext = type->GetMethodByName("opForNext");
    auto opForValue = type->GetMethodByName("opForValue");

    std::vector<asIScriptFunction *> opForValues;

    if (!opForValue)
    {
        for (int i = 0; ; i++)
        {
            auto f = type->GetMethodByName(fmt::format("opForValue{}", i).c_str());

            if (!f)
                break;

            opForValues.push_back(f);
        }
    }
    else
        opForValues.push_back(opForValue);

    if (index >= 0 && index < opForValues.size())
        opForValues = { opForValues[index] };

    // if we haven't got anything special yet, and we're
    // iterable, we'll show how many elements we have.
    // we'll also just assume the code isn't busted.
    int elementId = 0;
    
    cache.dbg->internal_execution = true;
    ctx->PushState();
    ctx->Prepare(opForBegin);
    ctx->SetObject(id.resolved);
    ctx->Execute();

    uint32_t rtn = ctx->GetReturnDWord();

    while (true)
    {
        ctx->Prepare(opForEnd);
        ctx->SetObject(id.resolved);
        ctx->SetArgDWord(0, rtn);
        ctx->Execute();
        bool finished = ctx->GetReturnByte();

        if (finished)
            break;

        int fv = 0;
        for (auto &opfv : opForValues)
        {
            ctx->Prepare(opfv);
            ctx->SetObject(id.resolved);
            ctx->SetArgDWord(0, rtn);
            ctx->Execute();

            void *addr = ctx->GetReturnAddress();
            int typeId = opfv->GetReturnTypeId();
            auto type = ctx->GetEngine()->GetTypeInfoById(typeId);
            std::unique_ptr<uint8_t[]> stackMemory;

            // non-heap stuff has to be copied somewhere
            // so the debugger can read it.
            // also has to be done for returned handles, because
            // asIDBResolvedVarAddr assumes handles are always dereferenced.
            if (!addr || (typeId & (asTYPEID_HANDLETOCONST | asTYPEID_OBJHANDLE)))
            {
                if ((!addr) == (typeId & (asTYPEID_HANDLETOCONST | asTYPEID_OBJHANDLE)))
                    __debugbreak();

                if (typeId & (asTYPEID_HANDLETOCONST | asTYPEID_OBJHANDLE))
                {
                    stackMemory = std::make_unique<uint8_t[]>(sizeof(addr));
                    memcpy(stackMemory.get(), ctx->GetAddressOfReturnValue(), sizeof(addr));
                }
                else
                {
                    size_t size = type ? type->GetSize() : ctx->GetEngine()->GetSizeOfPrimitiveType(typeId);
                    stackMemory = std::make_unique<uint8_t[]>(size);
                    memcpy(stackMemory.get(), ctx->GetAddressOfReturnValue(), size);
                }

                addr = stackMemory.get();
            }

            asIDBVarMap::iterator state;

            asIDBVarAddr elemId { typeId, false, addr };
            bool exists;
            state = cache.AddVarState(elemId, exists);

            if (!exists)
            {
                auto &propVar = state->second;

                if (stackMemory)
                    propVar.stackMemory = std::move(stackMemory);

                propVar.value = cache.evaluators.Evaluate(cache, elemId);
            }

            var.children.push_back(asIDBVarView { fmt::format(opForValues.size() == 1 ? "[{0}]" : "[{0},{1}]", elementId, fv), cache.GetTypeNameFromType({ typeId, asTM_NONE }), state });
            fv++;
        }
                
        ctx->Prepare(opForNext);
        ctx->SetObject(id.resolved);
        ctx->SetArgDWord(0, rtn);
        ctx->Execute();

        rtn = ctx->GetReturnDWord();

        elementId++;
    }

    ctx->PopState();
    cache.dbg->internal_execution = false;
}

const asIDBTypeEvaluator &asIDBTypeEvaluatorMap::GetEvaluator(asIDBCache &cache, const asIDBResolvedVarAddr &id) const
{
    // the only way the base address is null is if
    // it's uninitialized.
    static constexpr const asIDBUninitTypeEvaluator uninitType;
    static constexpr const asIDBNullTypeEvaluator nullType;

    if (id.source.address == nullptr)
        return uninitType;
    else if (id.resolved == nullptr)
        return nullType;

    // do we have a custom evaluator?
    if (auto f = evaluators.find(id.source.typeId & (asTYPEID_MASK_OBJECT | asTYPEID_MASK_SEQNBR)); f != evaluators.end())
        return *f->second.get();

    auto type = cache.ctx->GetEngine()->GetTypeInfoById(id.source.typeId);

    // are we a template?
    if (id.source.typeId & asTYPEID_TEMPLATE)
    {
        // fetch the base type, see if we have a
        // evaluator for that one
        auto baseType = cache.ctx->GetEngine()->GetTypeInfoByName(type->GetName());

        if (auto f = evaluators.find(baseType->GetTypeId() & (asTYPEID_MASK_OBJECT | asTYPEID_MASK_SEQNBR)); f != evaluators.end())
            return *f->second.get();
    }

    // we'll use the fall back evaluators.
    // check primitives first.
#define CHECK_PRIMITIVE_EVAL(asTypeId, cTypeName) \
    if (id.source.typeId == asTypeId) \
    { \
        static constexpr const asIDBPrimitiveTypeEvaluator<cTypeName> cTypeName##Type; \
        return cTypeName##Type; \
    }
    
    CHECK_PRIMITIVE_EVAL(asTYPEID_BOOL, bool);
    CHECK_PRIMITIVE_EVAL(asTYPEID_INT8, int8_t);
    CHECK_PRIMITIVE_EVAL(asTYPEID_INT16, int16_t);
    CHECK_PRIMITIVE_EVAL(asTYPEID_INT32, int32_t);
    CHECK_PRIMITIVE_EVAL(asTYPEID_INT64, int64_t);
    CHECK_PRIMITIVE_EVAL(asTYPEID_UINT8, uint8_t);
    CHECK_PRIMITIVE_EVAL(asTYPEID_UINT16, uint16_t);
    CHECK_PRIMITIVE_EVAL(asTYPEID_UINT32, uint32_t);
    CHECK_PRIMITIVE_EVAL(asTYPEID_UINT64, uint64_t);
    CHECK_PRIMITIVE_EVAL(asTYPEID_FLOAT, float);
    CHECK_PRIMITIVE_EVAL(asTYPEID_DOUBLE, double);

#undef CHECK_PRIMITIVE_EVAL

    if (type->GetFlags() & asOBJ_ENUM)
    {
        static constexpr const asIDBEnumTypeEvaluator enumType;
        return enumType;
    }
    else if (type->GetFlags() & asOBJ_FUNCDEF)
    {
        static constexpr const asIDBFuncDefTypeEvaluator funcdefType;
        return funcdefType;
    }

    // finally, just return the base one.
    static constexpr const asIDBObjectTypeEvaluator objectType;
    return objectType;
}

asIDBVarValue asIDBTypeEvaluatorMap::Evaluate(asIDBCache &cache, const asIDBResolvedVarAddr &id) const
{
    return GetEvaluator(cache, id).Evaluate(cache, id);
}

void asIDBTypeEvaluatorMap::Expand(asIDBCache &cache, const asIDBResolvedVarAddr &id, asIDBVarState &state) const
{
    GetEvaluator(cache, id).Expand(cache, id, state);
}

// Register an evaluator.
void asIDBTypeEvaluatorMap::Register(int typeId, std::unique_ptr<asIDBTypeEvaluator> evaluator)
{
    typeId &= asTYPEID_MASK_OBJECT | asTYPEID_MASK_SEQNBR;
    evaluators.insert_or_assign(typeId, std::move(evaluator));
}

/*static*/ void asIDBDebugger::LineCallback(asIScriptContext *ctx, asIDBDebugger *debugger)
{
    if (debugger->internal_execution)
        return;

    // we might not have an action - functions called from within
    // the debugger will never have this set.
    if (debugger->action != asIDBAction::None)
    {
        // Step Into just breaks on whatever happens to be next.
        if (debugger->action == asIDBAction::StepInto)
        {
            debugger->DebugBreak(ctx);
            return;
        }
        // Step Over breaks on the next line that is <= the
        // current stack level.
        else if (debugger->action == asIDBAction::StepOver)
        {
            if (ctx->GetCallstackSize() <= debugger->stack_size)
                debugger->DebugBreak(ctx);
            return;
        }
        // Step Out breaks on the next line that is < the
        // current stack level.
        else if (debugger->action == asIDBAction::StepOut)
        {
            if (ctx->GetCallstackSize() < debugger->stack_size)
                debugger->DebugBreak(ctx);
            return;
        }
    }

    // breakpoints are handled here. note that a single
    // breakpoint can be hit by multiple things on the same
    // line.
    bool break_from_bp = false;

    {
        std::scoped_lock lock(debugger->mutex);

        if (!debugger->breakpoints.empty())
        {
            const char *section = nullptr;
            int row = ctx->GetLineNumber(0, nullptr, &section);

            if (section && debugger->breakpoints.find(asIDBBreakpoint::FileLocation(asIDBBreakpointLocation { section, row })) != debugger->breakpoints.end())
                break_from_bp = true;
            else
            {
                // FIXME: this makes an std::string every time
                auto func = ctx->GetFunction(0);
                if (auto f = debugger->breakpoints.find(asIDBBreakpoint::Function(func->GetName())); f != debugger->breakpoints.end())
                {
                    debugger->breakpoints.erase(f);
                    break_from_bp = true;
                }
            }
        }
    }

    if (break_from_bp)
        debugger->DebugBreak(ctx);
}

void asIDBDebugger::HookContext(asIScriptContext *ctx)
{
    // TODO: is this safe to be called even if
    // the context is being switched?
    if (ctx->GetState() != asEXECUTION_EXCEPTION)
        ctx->SetLineCallback(asFUNCTION(asIDBDebugger::LineCallback), this, asCALL_CDECL);
}

void asIDBDebugger::DebugBreak(asIScriptContext *ctx)
{
    action = asIDBAction::None;
    {
        std::scoped_lock lock(mutex);
        std::unique_ptr<asIDBCache> new_cache = CreateCache(ctx);

        if (cache)
            new_cache->Restore(*cache);

        std::swap(cache, new_cache);
    }
    HookContext(ctx);
    Suspend();
}

bool asIDBDebugger::HasWork()
{
    std::scoped_lock lock(mutex);
    return !breakpoints.empty() && action == asIDBAction::None;
}

// debugger operations; these set the next breakpoint
// and call Resume.
void asIDBDebugger::StepInto()
{
    action = asIDBAction::StepInto;
    stack_size = cache->ctx->GetCallstackSize();
    Continue();
}

void asIDBDebugger::StepOver()
{
    action = asIDBAction::StepOver;
    stack_size = cache->ctx->GetCallstackSize();
    Continue();
}

void asIDBDebugger::StepOut()
{
    action = asIDBAction::StepOut;
    stack_size = cache->ctx->GetCallstackSize();
    Continue();
}

void asIDBDebugger::Continue()
{
    Resume();
}

bool asIDBDebugger::ToggleBreakpoint(std::string_view section, int line)
{
    asIDBBreakpoint bp = asIDBBreakpoint::FileLocation({ section, line });

    if (auto f = breakpoints.find(bp); f != breakpoints.end())
    {
        breakpoints.erase(f);
        return false;
    }
    else
    {
        breakpoints.insert(bp);
        return true;
    }
}

/*virtual*/ void asIDBDebugger::CacheSections(asIScriptModule *module)
{
    for (asUINT n = 0; n < module->GetFunctionCount(); n++)
    {
        const char *section = nullptr;
        module->GetFunctionByIndex(n)->GetDeclaredAt(&section, nullptr, nullptr);
        EnsureSectionCached(section, section);
    }
}

/*virtual*/ void asIDBDebugger::EnsureSectionCached(std::string_view section, std::string_view canonical)
{
    sections.insert({ section, canonical });
}