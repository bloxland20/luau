// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/Unifier.h"

#include "Luau/Common.h"
#include "Luau/RecursionCounter.h"
#include "Luau/Scope.h"
#include "Luau/TypePack.h"
#include "Luau/TypeUtils.h"
#include "Luau/TimeTrace.h"
#include "Luau/VisitTypeVar.h"

#include <algorithm>

LUAU_FASTINT(LuauTypeInferRecursionLimit);
LUAU_FASTINT(LuauTypeInferTypePackLoopLimit);
LUAU_FASTINTVARIABLE(LuauTypeInferIterationLimit, 2000);
LUAU_FASTFLAGVARIABLE(LuauTableSubtypingVariance2, false);
LUAU_FASTFLAGVARIABLE(LuauUnionHeuristic, false)
LUAU_FASTFLAGVARIABLE(LuauTableUnificationEarlyTest, false)
LUAU_FASTFLAGVARIABLE(LuauOccursCheckOkWithRecursiveFunctions, false)
LUAU_FASTFLAGVARIABLE(LuauExtendedTypeMismatchError, false)
LUAU_FASTFLAG(LuauSingletonTypes)
LUAU_FASTFLAGVARIABLE(LuauExtendedClassMismatchError, false)
LUAU_FASTFLAG(LuauErrorRecoveryType);
LUAU_FASTFLAG(LuauProperTypeLevels);
LUAU_FASTFLAGVARIABLE(LuauExtendedUnionMismatchError, false)
LUAU_FASTFLAGVARIABLE(LuauExtendedFunctionMismatchError, false)

namespace Luau
{

struct PromoteTypeLevels
{
    TxnLog& log;
    TypeLevel minLevel;

    explicit PromoteTypeLevels(TxnLog& log, TypeLevel minLevel)
        : log(log)
        , minLevel(minLevel)
    {}

    template <typename TID, typename T>
    void promote(TID ty, T* t)
    {
        LUAU_ASSERT(t);
        if (minLevel.subsumesStrict(t->level))
        {
            log(ty);
            t->level = minLevel;
        }
    }

    template<typename TID>
    void cycle(TID) {}

    template<typename TID, typename T>
    bool operator()(TID, const T&)
    {
        return true;
    }

    bool operator()(TypeId ty, const FreeTypeVar&)
    {
        promote(ty, getMutable<FreeTypeVar>(ty));
        return true;
    }

    bool operator()(TypeId ty, const FunctionTypeVar&)
    {
        promote(ty, getMutable<FunctionTypeVar>(ty));
        return true;
    }

    bool operator()(TypeId ty, const TableTypeVar&)
    {
        promote(ty, getMutable<TableTypeVar>(ty));
        return true;
    }

    bool operator()(TypePackId tp, const FreeTypePack&)
    {
        promote(tp, getMutable<FreeTypePack>(tp));
        return true;
    }
};

void promoteTypeLevels(TxnLog& log, TypeLevel minLevel, TypeId ty)
{
    PromoteTypeLevels ptl{log, minLevel};
    DenseHashSet<void*> seen{nullptr};
    visitTypeVarOnce(ty, ptl, seen);
}

void promoteTypeLevels(TxnLog& log, TypeLevel minLevel, TypePackId tp)
{
    PromoteTypeLevels ptl{log, minLevel};
    DenseHashSet<void*> seen{nullptr};
    visitTypeVarOnce(tp, ptl, seen);
}

struct SkipCacheForType
{
    SkipCacheForType(const DenseHashMap<TypeId, bool>& skipCacheForType)
        : skipCacheForType(skipCacheForType)
    {
    }

    void cycle(TypeId) {}
    void cycle(TypePackId) {}

    bool operator()(TypeId ty, const FreeTypeVar& ftv)
    {
        result = true;
        return false;
    }

    bool operator()(TypeId ty, const BoundTypeVar& btv)
    {
        result = true;
        return false;
    }

    bool operator()(TypeId ty, const GenericTypeVar& btv)
    {
        result = true;
        return false;
    }

    bool operator()(TypeId ty, const TableTypeVar&)
    {
        TableTypeVar& ttv = *getMutable<TableTypeVar>(ty);

        if (ttv.boundTo)
        {
            result = true;
            return false;
        }

        if (ttv.state != TableState::Sealed)
        {
            result = true;
            return false;
        }

        return true;
    }

    template<typename T>
    bool operator()(TypeId ty, const T& t)
    {
        const bool* prev = skipCacheForType.find(ty);

        if (prev && *prev)
        {
            result = true;
            return false;
        }

        return true;
    }

    template<typename T>
    bool operator()(TypePackId, const T&)
    {
        return true;
    }

    bool operator()(TypePackId tp, const FreeTypePack& ftp)
    {
        result = true;
        return false;
    }

    bool operator()(TypePackId tp, const BoundTypePack& ftp)
    {
        result = true;
        return false;
    }

    bool operator()(TypePackId tp, const GenericTypePack& ftp)
    {
        result = true;
        return false;
    }

    const DenseHashMap<TypeId, bool>& skipCacheForType;
    bool result = false;
};

static std::optional<TypeError> hasUnificationTooComplex(const ErrorVec& errors)
{
    auto isUnificationTooComplex = [](const TypeError& te) {
        return nullptr != get<UnificationTooComplex>(te);
    };

    auto it = std::find_if(errors.begin(), errors.end(), isUnificationTooComplex);
    if (it == errors.end())
        return std::nullopt;
    else
        return *it;
}

// Used for tagged union matching heuristic, returns first singleton type field
static std::optional<std::pair<Luau::Name, const SingletonTypeVar*>> getTableMatchTag(TypeId type)
{
    LUAU_ASSERT(FFlag::LuauExtendedUnionMismatchError);

    type = follow(type);

    if (auto ttv = get<TableTypeVar>(type))
    {
        for (auto&& [name, prop] : ttv->props)
        {
            if (auto sing = get<SingletonTypeVar>(follow(prop.type)))
                return {{name, sing}};
        }
    }
    else if (auto mttv = get<MetatableTypeVar>(type))
    {
        return getTableMatchTag(mttv->table);
    }

    return std::nullopt;
}

Unifier::Unifier(TypeArena* types, Mode mode, ScopePtr globalScope, const Location& location, Variance variance, UnifierSharedState& sharedState)
    : types(types)
    , mode(mode)
    , globalScope(std::move(globalScope))
    , location(location)
    , variance(variance)
    , sharedState(sharedState)
{
    LUAU_ASSERT(sharedState.iceHandler);
}

Unifier::Unifier(TypeArena* types, Mode mode, ScopePtr globalScope, std::vector<std::pair<TypeId, TypeId>>* sharedSeen, const Location& location,
    Variance variance, UnifierSharedState& sharedState)
    : types(types)
    , mode(mode)
    , globalScope(std::move(globalScope))
    , log(sharedSeen)
    , location(location)
    , variance(variance)
    , sharedState(sharedState)
{
    LUAU_ASSERT(sharedState.iceHandler);
}

void Unifier::tryUnify(TypeId superTy, TypeId subTy, bool isFunctionCall, bool isIntersection)
{
    sharedState.counters.iterationCount = 0;

    tryUnify_(superTy, subTy, isFunctionCall, isIntersection);
}

void Unifier::tryUnify_(TypeId superTy, TypeId subTy, bool isFunctionCall, bool isIntersection)
{
    RecursionLimiter _ra(&sharedState.counters.recursionCount, FInt::LuauTypeInferRecursionLimit);

    ++sharedState.counters.iterationCount;

    if (FInt::LuauTypeInferIterationLimit > 0 && FInt::LuauTypeInferIterationLimit < sharedState.counters.iterationCount)
    {
        errors.push_back(TypeError{location, UnificationTooComplex{}});
        return;
    }

    superTy = follow(superTy);
    subTy = follow(subTy);

    if (superTy == subTy)
        return;

    auto l = getMutable<FreeTypeVar>(superTy);
    auto r = getMutable<FreeTypeVar>(subTy);

    if (l && r && l->level.subsumes(r->level))
    {
        occursCheck(subTy, superTy);

        // The occurrence check might have caused superTy no longer to be a free type
        if (!get<ErrorTypeVar>(subTy))
        {
            log(subTy);
            *asMutable(subTy) = BoundTypeVar(superTy);
        }

        return;
    }
    else if (l && r)
    {
        if (!FFlag::LuauErrorRecoveryType)
            log(superTy);
        occursCheck(superTy, subTy);
        r->level = min(r->level, l->level);

        // The occurrence check might have caused superTy no longer to be a free type
        if (!FFlag::LuauErrorRecoveryType)
            *asMutable(superTy) = BoundTypeVar(subTy);
        else if (!get<ErrorTypeVar>(superTy))
        {
            log(superTy);
            *asMutable(superTy) = BoundTypeVar(subTy);
        }

        return;
    }
    else if (l)
    {
        occursCheck(superTy, subTy);

        TypeLevel superLevel = l->level;

        // Unification can't change the level of a generic.
        auto rightGeneric = get<GenericTypeVar>(subTy);
        if (rightGeneric && !rightGeneric->level.subsumes(superLevel))
        {
            // TODO: a more informative error message? CLI-39912
            errors.push_back(TypeError{location, GenericError{"Generic subtype escaping scope"}});
            return;
        }

        // The occurrence check might have caused superTy no longer to be a free type
        if (!get<ErrorTypeVar>(superTy))
        {
            if (FFlag::LuauProperTypeLevels)
                promoteTypeLevels(log, superLevel, subTy);
            else if (auto rightLevel = getMutableLevel(subTy))
            {
                if (!rightLevel->subsumes(l->level))
                    *rightLevel = l->level;
            }

            log(superTy);
            *asMutable(superTy) = BoundTypeVar(subTy);
        }

        return;
    }
    else if (r)
    {
        TypeLevel subLevel = r->level;

        occursCheck(subTy, superTy);

        // Unification can't change the level of a generic.
        auto leftGeneric = get<GenericTypeVar>(superTy);
        if (leftGeneric && !leftGeneric->level.subsumes(r->level))
        {
            // TODO: a more informative error message? CLI-39912
            errors.push_back(TypeError{location, GenericError{"Generic supertype escaping scope"}});
            return;
        }

        if (!get<ErrorTypeVar>(subTy))
        {
            if (FFlag::LuauProperTypeLevels)
                promoteTypeLevels(log, subLevel, superTy);

            if (auto superLevel = getMutableLevel(superTy))
            {
                if (!superLevel->subsumes(r->level))
                {
                    log(superTy);
                    *superLevel = r->level;
                }
            }

            log(subTy);
            *asMutable(subTy) = BoundTypeVar(superTy);
        }

        return;
    }

    if (get<ErrorTypeVar>(superTy) || get<AnyTypeVar>(superTy))
        return tryUnifyWithAny(superTy, subTy);

    if (get<ErrorTypeVar>(subTy) || get<AnyTypeVar>(subTy))
        return tryUnifyWithAny(subTy, superTy);

    bool cacheEnabled = !isFunctionCall && !isIntersection;
    auto& cache = sharedState.cachedUnify;

    // What if the types are immutable and we proved their relation before
    if (cacheEnabled && cache.contains({superTy, subTy}) && (variance == Covariant || cache.contains({subTy, superTy})))
        return;

    // If we have seen this pair of types before, we are currently recursing into cyclic types.
    // Here, we assume that the types unify.  If they do not, we will find out as we roll back
    // the stack.

    if (log.haveSeen(superTy, subTy))
        return;

    log.pushSeen(superTy, subTy);

    if (const UnionTypeVar* uv = get<UnionTypeVar>(subTy))
    {
        // A | B <: T if A <: T and B <: T
        bool failed = false;
        std::optional<TypeError> unificationTooComplex;
        std::optional<TypeError> firstFailedOption;

        size_t count = uv->options.size();
        size_t i = 0;

        for (TypeId type : uv->options)
        {
            Unifier innerState = makeChildUnifier();
            innerState.tryUnify_(superTy, type);

            if (auto e = hasUnificationTooComplex(innerState.errors))
                unificationTooComplex = e;
            else if (!innerState.errors.empty())
            {
                // 'nil' option is skipped from extended report because we present the type in a special way - 'T?'
                if (FFlag::LuauExtendedTypeMismatchError && !firstFailedOption && !isNil(type))
                    firstFailedOption = {innerState.errors.front()};

                failed = true;
            }

            if (i != count - 1)
                innerState.log.rollback();
            else
                log.concat(std::move(innerState.log));

            ++i;
        }

        if (unificationTooComplex)
            errors.push_back(*unificationTooComplex);
        else if (failed)
        {
            if (FFlag::LuauExtendedTypeMismatchError && firstFailedOption)
                errors.push_back(TypeError{location, TypeMismatch{superTy, subTy, "Not all union options are compatible.", *firstFailedOption}});
            else
                errors.push_back(TypeError{location, TypeMismatch{superTy, subTy}});
        }
    }
    else if (const UnionTypeVar* uv = get<UnionTypeVar>(superTy))
    {
        // T <: A | B if T <: A or T <: B
        bool found = false;
        std::optional<TypeError> unificationTooComplex;

        size_t failedOptionCount = 0;
        std::optional<TypeError> failedOption;

        bool foundHeuristic = false;
        size_t startIndex = 0;

        if (FFlag::LuauUnionHeuristic)
        {
            if (const std::string* subName = getName(subTy))
            {
                for (size_t i = 0; i < uv->options.size(); ++i)
                {
                    const std::string* optionName = getName(uv->options[i]);
                    if (optionName && *optionName == *subName)
                    {
                        foundHeuristic = true;
                        startIndex = i;
                        break;
                    }
                }
            }

            if (FFlag::LuauExtendedUnionMismatchError)
            {
                if (auto subMatchTag = getTableMatchTag(subTy))
                {
                    for (size_t i = 0; i < uv->options.size(); ++i)
                    {
                        auto optionMatchTag = getTableMatchTag(uv->options[i]);
                        if (optionMatchTag && optionMatchTag->first == subMatchTag->first && *optionMatchTag->second == *subMatchTag->second)
                        {
                            foundHeuristic = true;
                            startIndex = i;
                            break;
                        }
                    }
                }
            }

            if (!foundHeuristic && cacheEnabled)
            {
                for (size_t i = 0; i < uv->options.size(); ++i)
                {
                    TypeId type = uv->options[i];

                    if (cache.contains({type, subTy}) && (variance == Covariant || cache.contains({subTy, type})))
                    {
                        startIndex = i;
                        break;
                    }
                }
            }
        }

        for (size_t i = 0; i < uv->options.size(); ++i)
        {
            TypeId type = uv->options[(i + startIndex) % uv->options.size()];
            Unifier innerState = makeChildUnifier();
            innerState.tryUnify_(type, subTy, isFunctionCall);

            if (innerState.errors.empty())
            {
                found = true;
                log.concat(std::move(innerState.log));
                break;
            }
            else if (auto e = hasUnificationTooComplex(innerState.errors))
            {
                unificationTooComplex = e;
            }
            else if (FFlag::LuauExtendedUnionMismatchError && !isNil(type))
            {
                failedOptionCount++;

                if (!failedOption)
                    failedOption = {innerState.errors.front()};
            }

            innerState.log.rollback();
        }

        if (unificationTooComplex)
        {
            errors.push_back(*unificationTooComplex);
        }
        else if (!found)
        {
            if (FFlag::LuauExtendedUnionMismatchError && (failedOptionCount == 1 || foundHeuristic) && failedOption)
                errors.push_back(
                    TypeError{location, TypeMismatch{superTy, subTy, "None of the union options are compatible. For example:", *failedOption}});
            else if (FFlag::LuauExtendedTypeMismatchError)
                errors.push_back(TypeError{location, TypeMismatch{superTy, subTy, "none of the union options are compatible"}});
            else
                errors.push_back(TypeError{location, TypeMismatch{superTy, subTy}});
        }
    }
    else if (const IntersectionTypeVar* uv = get<IntersectionTypeVar>(superTy))
    {
        if (FFlag::LuauExtendedTypeMismatchError)
        {
            std::optional<TypeError> unificationTooComplex;
            std::optional<TypeError> firstFailedOption;

            // T <: A & B if A <: T and B <: T
            for (TypeId type : uv->parts)
            {
                Unifier innerState = makeChildUnifier();
                innerState.tryUnify_(type, subTy, /*isFunctionCall*/ false, /*isIntersection*/ true);

                if (auto e = hasUnificationTooComplex(innerState.errors))
                    unificationTooComplex = e;
                else if (!innerState.errors.empty())
                {
                    if (!firstFailedOption)
                        firstFailedOption = {innerState.errors.front()};
                }

                log.concat(std::move(innerState.log));
            }

            if (unificationTooComplex)
                errors.push_back(*unificationTooComplex);
            else if (firstFailedOption)
                errors.push_back(TypeError{location, TypeMismatch{superTy, subTy, "Not all intersection parts are compatible.", *firstFailedOption}});
        }
        else
        {
            // T <: A & B if A <: T and B <: T
            for (TypeId type : uv->parts)
            {
                tryUnify_(type, subTy, /*isFunctionCall*/ false, /*isIntersection*/ true);
            }
        }
    }
    else if (const IntersectionTypeVar* uv = get<IntersectionTypeVar>(subTy))
    {
        // A & B <: T if T <: A or T <: B
        bool found = false;
        std::optional<TypeError> unificationTooComplex;

        size_t startIndex = 0;

        if (cacheEnabled)
        {
            for (size_t i = 0; i < uv->parts.size(); ++i)
            {
                TypeId type = uv->parts[i];

                if (cache.contains({superTy, type}) && (variance == Covariant || cache.contains({type, superTy})))
                {
                    startIndex = i;
                    break;
                }
            }
        }

        for (size_t i = 0; i < uv->parts.size(); ++i)
        {
            TypeId type = uv->parts[(i + startIndex) % uv->parts.size()];
            Unifier innerState = makeChildUnifier();
            innerState.tryUnify_(superTy, type, isFunctionCall);

            if (innerState.errors.empty())
            {
                found = true;
                log.concat(std::move(innerState.log));
                break;
            }
            else if (auto e = hasUnificationTooComplex(innerState.errors))
            {
                unificationTooComplex = e;
            }

            innerState.log.rollback();
        }

        if (unificationTooComplex)
            errors.push_back(*unificationTooComplex);
        else if (!found)
        {
            if (FFlag::LuauExtendedTypeMismatchError)
                errors.push_back(TypeError{location, TypeMismatch{superTy, subTy, "none of the intersection parts are compatible"}});
            else
                errors.push_back(TypeError{location, TypeMismatch{superTy, subTy}});
        }
    }
    else if (get<PrimitiveTypeVar>(superTy) && get<PrimitiveTypeVar>(subTy))
        tryUnifyPrimitives(superTy, subTy);

    else if (FFlag::LuauSingletonTypes && (get<PrimitiveTypeVar>(superTy) || get<SingletonTypeVar>(superTy)) && get<SingletonTypeVar>(subTy))
        tryUnifySingletons(superTy, subTy);

    else if (get<FunctionTypeVar>(superTy) && get<FunctionTypeVar>(subTy))
        tryUnifyFunctions(superTy, subTy, isFunctionCall);

    else if (get<TableTypeVar>(superTy) && get<TableTypeVar>(subTy))
    {
        tryUnifyTables(superTy, subTy, isIntersection);

        if (cacheEnabled && errors.empty())
            cacheResult(superTy, subTy);
    }

    // tryUnifyWithMetatable assumes its first argument is a MetatableTypeVar. The check is otherwise symmetrical.
    else if (get<MetatableTypeVar>(superTy))
        tryUnifyWithMetatable(superTy, subTy, /*reversed*/ false);
    else if (get<MetatableTypeVar>(subTy))
        tryUnifyWithMetatable(subTy, superTy, /*reversed*/ true);

    else if (get<ClassTypeVar>(superTy))
        tryUnifyWithClass(superTy, subTy, /*reversed*/ false);

    // Unification of nonclasses with classes is almost, but not quite symmetrical.
    // The order in which we perform this test is significant in the case that both types are classes.
    else if (get<ClassTypeVar>(subTy))
        tryUnifyWithClass(superTy, subTy, /*reversed*/ true);

    else
        errors.push_back(TypeError{location, TypeMismatch{superTy, subTy}});

    log.popSeen(superTy, subTy);
}

void Unifier::cacheResult(TypeId superTy, TypeId subTy)
{
    bool* superTyInfo = sharedState.skipCacheForType.find(superTy);

    if (superTyInfo && *superTyInfo)
        return;

    bool* subTyInfo = sharedState.skipCacheForType.find(subTy);

    if (subTyInfo && *subTyInfo)
        return;

    auto skipCacheFor = [this](TypeId ty) {
        SkipCacheForType visitor{sharedState.skipCacheForType};
        visitTypeVarOnce(ty, visitor, sharedState.seenAny);

        sharedState.skipCacheForType[ty] = visitor.result;

        return visitor.result;
    };

    if (!superTyInfo && skipCacheFor(superTy))
        return;

    if (!subTyInfo && skipCacheFor(subTy))
        return;

    sharedState.cachedUnify.insert({superTy, subTy});

    if (variance == Invariant)
        sharedState.cachedUnify.insert({subTy, superTy});
}

struct WeirdIter
{
    TypePackId packId;
    const TypePack* pack;
    size_t index;
    bool growing;
    TypeLevel level;

    WeirdIter(TypePackId packId)
        : packId(packId)
        , pack(get<TypePack>(packId))
        , index(0)
        , growing(false)
    {
        while (pack && pack->head.empty() && pack->tail)
        {
            packId = *pack->tail;
            pack = get<TypePack>(packId);
        }
    }

    WeirdIter(const WeirdIter&) = default;

    const TypeId& operator*()
    {
        LUAU_ASSERT(good());
        return pack->head[index];
    }

    bool good() const
    {
        return pack != nullptr && index < pack->head.size();
    }

    bool advance()
    {
        if (!pack)
            return good();

        if (index < pack->head.size())
            ++index;

        if (growing || index < pack->head.size())
            return good();

        if (pack->tail)
        {
            packId = follow(*pack->tail);
            pack = get<TypePack>(packId);
            index = 0;
        }

        return good();
    }

    bool canGrow() const
    {
        return nullptr != get<Unifiable::Free>(packId);
    }

    void grow(TypePackId newTail)
    {
        LUAU_ASSERT(canGrow());
        level = get<Unifiable::Free>(packId)->level;
        *asMutable(packId) = Unifiable::Bound<TypePackId>(newTail);
        packId = newTail;
        pack = get<TypePack>(newTail);
        index = 0;
        growing = true;
    }
};

ErrorVec Unifier::canUnify(TypeId superTy, TypeId subTy)
{
    Unifier s = makeChildUnifier();
    s.tryUnify_(superTy, subTy);
    s.log.rollback();
    return s.errors;
}

ErrorVec Unifier::canUnify(TypePackId superTy, TypePackId subTy, bool isFunctionCall)
{
    Unifier s = makeChildUnifier();
    s.tryUnify_(superTy, subTy, isFunctionCall);
    s.log.rollback();
    return s.errors;
}

void Unifier::tryUnify(TypePackId superTp, TypePackId subTp, bool isFunctionCall)
{
    sharedState.counters.iterationCount = 0;

    tryUnify_(superTp, subTp, isFunctionCall);
}

/*
 * This is quite tricky: we are walking two rope-like structures and unifying corresponding elements.
 * If one is longer than the other, but the short end is free, we grow it to the required length.
 */
void Unifier::tryUnify_(TypePackId superTp, TypePackId subTp, bool isFunctionCall)
{
    RecursionLimiter _ra(&sharedState.counters.recursionCount, FInt::LuauTypeInferRecursionLimit);

    ++sharedState.counters.iterationCount;

    if (FInt::LuauTypeInferIterationLimit > 0 && FInt::LuauTypeInferIterationLimit < sharedState.counters.iterationCount)
    {
        errors.push_back(TypeError{location, UnificationTooComplex{}});
        return;
    }

    superTp = follow(superTp);
    subTp = follow(subTp);

    while (auto r = get<TypePack>(subTp))
    {
        if (r->head.empty() && r->tail)
            subTp = follow(*r->tail);
        else
            break;
    }

    while (auto l = get<TypePack>(superTp))
    {
        if (l->head.empty() && l->tail)
            superTp = follow(*l->tail);
        else
            break;
    }

    if (superTp == subTp)
        return;

    if (get<Unifiable::Free>(superTp))
    {
        occursCheck(superTp, subTp);

        // The occurrence check might have caused superTp no longer to be a free type
        if (!get<ErrorTypeVar>(superTp))
        {
            log(superTp);
            *asMutable(superTp) = Unifiable::Bound<TypePackId>(subTp);
        }
    }
    else if (get<Unifiable::Free>(subTp))
    {
        occursCheck(subTp, superTp);

        // The occurrence check might have caused superTp no longer to be a free type
        if (!get<ErrorTypeVar>(subTp))
        {
            log(subTp);
            *asMutable(subTp) = Unifiable::Bound<TypePackId>(superTp);
        }
    }

    else if (get<Unifiable::Error>(superTp))
        tryUnifyWithAny(superTp, subTp);

    else if (get<Unifiable::Error>(subTp))
        tryUnifyWithAny(subTp, superTp);

    else if (get<VariadicTypePack>(superTp))
        tryUnifyVariadics(superTp, subTp, false);
    else if (get<VariadicTypePack>(subTp))
        tryUnifyVariadics(subTp, superTp, true);

    else if (get<TypePack>(superTp) && get<TypePack>(subTp))
    {
        auto l = get<TypePack>(superTp);
        auto r = get<TypePack>(subTp);

        // If the size of two heads does not match, but both packs have free tail
        // We set the sentinel variable to say so to avoid growing it forever.
        auto [superTypes, superTail] = flatten(superTp);
        auto [subTypes, subTail] = flatten(subTp);

        bool noInfiniteGrowth =
            (superTypes.size() != subTypes.size()) && (superTail && get<FreeTypePack>(*superTail)) && (subTail && get<FreeTypePack>(*subTail));

        auto superIter = WeirdIter{superTp};
        auto subIter = WeirdIter{subTp};

        auto mkFreshType = [this](TypeLevel level) {
            return types->freshType(level);
        };

        const TypePackId emptyTp = types->addTypePack(TypePack{{}, std::nullopt});

        int loopCount = 0;

        do
        {
            if (FInt::LuauTypeInferTypePackLoopLimit > 0 && loopCount >= FInt::LuauTypeInferTypePackLoopLimit)
                ice("Detected possibly infinite TypePack growth");

            ++loopCount;

            if (superIter.good() && subIter.growing)
                asMutable(subIter.pack)->head.push_back(mkFreshType(subIter.level));

            if (subIter.good() && superIter.growing)
                asMutable(superIter.pack)->head.push_back(mkFreshType(superIter.level));

            if (superIter.good() && subIter.good())
            {
                tryUnify_(*superIter, *subIter);

                if (FFlag::LuauExtendedFunctionMismatchError && !errors.empty() && !firstPackErrorPos)
                    firstPackErrorPos = loopCount;

                superIter.advance();
                subIter.advance();
                continue;
            }

            // If both are at the end, we're done
            if (!superIter.good() && !subIter.good())
            {
                const bool lFreeTail = l->tail && get<FreeTypePack>(follow(*l->tail)) != nullptr;
                const bool rFreeTail = r->tail && get<FreeTypePack>(follow(*r->tail)) != nullptr;
                if (lFreeTail && rFreeTail)
                    tryUnify_(*l->tail, *r->tail);
                else if (lFreeTail)
                    tryUnify_(*l->tail, emptyTp);
                else if (rFreeTail)
                    tryUnify_(*r->tail, emptyTp);

                break;
            }

            // If both tails are free, bind one to the other and call it a day
            if (superIter.canGrow() && subIter.canGrow())
                return tryUnify_(*superIter.pack->tail, *subIter.pack->tail);

            // If just one side is free on its tail, grow it to fit the other side.
            // FIXME: The tail-most tail of the growing pack should be the same as the tail-most tail of the non-growing pack.
            if (superIter.canGrow())
                superIter.grow(types->addTypePack(TypePackVar(TypePack{})));

            else if (subIter.canGrow())
                subIter.grow(types->addTypePack(TypePackVar(TypePack{})));

            else
            {
                // A union type including nil marks an optional argument
                if (superIter.good() && isOptional(*superIter))
                {
                    superIter.advance();
                    continue;
                }
                else if (subIter.good() && isOptional(*subIter))
                {
                    subIter.advance();
                    continue;
                }

                // In nonstrict mode, any also marks an optional argument.
                else if (superIter.good() && isNonstrictMode() && get<AnyTypeVar>(follow(*superIter)))
                {
                    superIter.advance();
                    continue;
                }

                if (get<VariadicTypePack>(superIter.packId))
                {
                    tryUnifyVariadics(superIter.packId, subIter.packId, false, int(subIter.index));
                    return;
                }

                if (get<VariadicTypePack>(subIter.packId))
                {
                    tryUnifyVariadics(subIter.packId, superIter.packId, true, int(superIter.index));
                    return;
                }

                if (!isFunctionCall && subIter.good())
                {
                    // Sometimes it is ok to pass too many arguments
                    return;
                }

                // This is a bit weird because we don't actually know expected vs actual.  We just know
                // subtype vs supertype.  If we are checking the values returned by a function, we swap
                // these to produce the expected error message.
                size_t expectedSize = size(superTp);
                size_t actualSize = size(subTp);
                if (ctx == CountMismatch::Result)
                    std::swap(expectedSize, actualSize);
                errors.push_back(TypeError{location, CountMismatch{expectedSize, actualSize, ctx}});

                while (superIter.good())
                {
                    tryUnify_(getSingletonTypes().errorRecoveryType(), *superIter);
                    superIter.advance();
                }

                while (subIter.good())
                {
                    tryUnify_(getSingletonTypes().errorRecoveryType(), *subIter);
                    subIter.advance();
                }

                return;
            }

        } while (!noInfiniteGrowth);
    }
    else
    {
        errors.push_back(TypeError{location, GenericError{"Failed to unify type packs"}});
    }
}

void Unifier::tryUnifyPrimitives(TypeId superTy, TypeId subTy)
{
    const PrimitiveTypeVar* lp = get<PrimitiveTypeVar>(superTy);
    const PrimitiveTypeVar* rp = get<PrimitiveTypeVar>(subTy);
    if (!lp || !rp)
        ice("passed non primitive types to unifyPrimitives");

    if (lp->type != rp->type)
        errors.push_back(TypeError{location, TypeMismatch{superTy, subTy}});
}

void Unifier::tryUnifySingletons(TypeId superTy, TypeId subTy)
{
    const PrimitiveTypeVar* lp = get<PrimitiveTypeVar>(superTy);
    const SingletonTypeVar* ls = get<SingletonTypeVar>(superTy);
    const SingletonTypeVar* rs = get<SingletonTypeVar>(subTy);

    if ((!lp && !ls) || !rs)
        ice("passed non singleton/primitive types to unifySingletons");

    if (ls && *ls == *rs)
        return;

    if (lp && lp->type == PrimitiveTypeVar::Boolean && get<BoolSingleton>(rs) && variance == Covariant)
        return;

    if (lp && lp->type == PrimitiveTypeVar::String && get<StringSingleton>(rs) && variance == Covariant)
        return;

    errors.push_back(TypeError{location, TypeMismatch{superTy, subTy}});
}

void Unifier::tryUnifyFunctions(TypeId superTy, TypeId subTy, bool isFunctionCall)
{
    FunctionTypeVar* lf = getMutable<FunctionTypeVar>(superTy);
    FunctionTypeVar* rf = getMutable<FunctionTypeVar>(subTy);
    if (!lf || !rf)
        ice("passed non-function types to unifyFunction");

    size_t numGenerics = lf->generics.size();
    if (numGenerics != rf->generics.size())
    {
        numGenerics = std::min(lf->generics.size(), rf->generics.size());

        if (FFlag::LuauExtendedFunctionMismatchError)
            errors.push_back(TypeError{location, TypeMismatch{superTy, subTy, "different number of generic type parameters"}});
        else
            errors.push_back(TypeError{location, TypeMismatch{superTy, subTy}});
    }

    size_t numGenericPacks = lf->genericPacks.size();
    if (numGenericPacks != rf->genericPacks.size())
    {
        numGenericPacks = std::min(lf->genericPacks.size(), rf->genericPacks.size());

        if (FFlag::LuauExtendedFunctionMismatchError)
            errors.push_back(TypeError{location, TypeMismatch{superTy, subTy, "different number of generic type pack parameters"}});
        else
            errors.push_back(TypeError{location, TypeMismatch{superTy, subTy}});
    }

    for (size_t i = 0; i < numGenerics; i++)
        log.pushSeen(lf->generics[i], rf->generics[i]);

    CountMismatch::Context context = ctx;

    if (!isFunctionCall)
    {
        Unifier innerState = makeChildUnifier();

        if (FFlag::LuauExtendedFunctionMismatchError)
        {
            innerState.ctx = CountMismatch::Arg;
            innerState.tryUnify_(rf->argTypes, lf->argTypes, isFunctionCall);

            bool reported = !innerState.errors.empty();

            if (auto e = hasUnificationTooComplex(innerState.errors))
                errors.push_back(*e);
            else if (!innerState.errors.empty() && innerState.firstPackErrorPos)
                errors.push_back(
                    TypeError{location, TypeMismatch{superTy, subTy, format("Argument #%d type is not compatible.", *innerState.firstPackErrorPos),
                                            innerState.errors.front()}});
            else if (!innerState.errors.empty())
                errors.push_back(TypeError{location, TypeMismatch{superTy, subTy, "", innerState.errors.front()}});

            innerState.ctx = CountMismatch::Result;
            innerState.tryUnify_(lf->retType, rf->retType);

            if (!reported)
            {
                if (auto e = hasUnificationTooComplex(innerState.errors))
                    errors.push_back(*e);
                else if (!innerState.errors.empty() && size(lf->retType) == 1 && finite(lf->retType))
                    errors.push_back(TypeError{location, TypeMismatch{superTy, subTy, "Return type is not compatible.", innerState.errors.front()}});
                else if (!innerState.errors.empty() && innerState.firstPackErrorPos)
                    errors.push_back(
                        TypeError{location, TypeMismatch{superTy, subTy, format("Return #%d type is not compatible.", *innerState.firstPackErrorPos),
                                                innerState.errors.front()}});
                else if (!innerState.errors.empty())
                    errors.push_back(TypeError{location, TypeMismatch{superTy, subTy, "", innerState.errors.front()}});
            }
        }
        else
        {
            ctx = CountMismatch::Arg;
            innerState.tryUnify_(rf->argTypes, lf->argTypes, isFunctionCall);

            ctx = CountMismatch::Result;
            innerState.tryUnify_(lf->retType, rf->retType);

            checkChildUnifierTypeMismatch(innerState.errors, superTy, subTy);
        }

        log.concat(std::move(innerState.log));
    }
    else
    {
        ctx = CountMismatch::Arg;
        tryUnify_(rf->argTypes, lf->argTypes, isFunctionCall);

        ctx = CountMismatch::Result;
        tryUnify_(lf->retType, rf->retType);
    }

    if (lf->definition && !rf->definition && !subTy->persistent)
    {
        rf->definition = lf->definition;
    }
    else if (!lf->definition && rf->definition && !superTy->persistent)
    {
        lf->definition = rf->definition;
    }

    ctx = context;

    for (int i = int(numGenerics) - 1; 0 <= i; i--)
        log.popSeen(lf->generics[i], rf->generics[i]);
}

namespace
{

struct Resetter
{
    explicit Resetter(Variance* variance)
        : oldValue(*variance)
        , variance(variance)
    {
    }

    Variance oldValue;
    Variance* variance;

    ~Resetter()
    {
        *variance = oldValue;
    }
};

} // namespace

void Unifier::tryUnifyTables(TypeId left, TypeId right, bool isIntersection)
{
    if (!FFlag::LuauTableSubtypingVariance2)
        return DEPRECATED_tryUnifyTables(left, right, isIntersection);

    TableTypeVar* lt = getMutable<TableTypeVar>(left);
    TableTypeVar* rt = getMutable<TableTypeVar>(right);
    if (!lt || !rt)
        ice("passed non-table types to unifyTables");

    std::vector<std::string> missingProperties;
    std::vector<std::string> extraProperties;

    // Optimization: First test that the property sets are compatible without doing any recursive unification
    if (FFlag::LuauTableUnificationEarlyTest && !rt->indexer && rt->state != TableState::Free)
    {
        for (const auto& [propName, superProp] : lt->props)
        {
            auto subIter = rt->props.find(propName);
            if (subIter == rt->props.end() && !isOptional(superProp.type) && !get<AnyTypeVar>(follow(superProp.type)))
                missingProperties.push_back(propName);
        }

        if (!missingProperties.empty())
        {
            errors.push_back(TypeError{location, MissingProperties{left, right, std::move(missingProperties)}});
            return;
        }
    }

    // And vice versa if we're invariant
    if (FFlag::LuauTableUnificationEarlyTest && variance == Invariant && !lt->indexer && lt->state != TableState::Unsealed &&
        lt->state != TableState::Free)
    {
        for (const auto& [propName, subProp] : rt->props)
        {
            auto superIter = lt->props.find(propName);
            if (superIter == lt->props.end() && !isOptional(subProp.type) && !get<AnyTypeVar>(follow(subProp.type)))
                extraProperties.push_back(propName);
        }

        if (!extraProperties.empty())
        {
            errors.push_back(TypeError{location, MissingProperties{left, right, std::move(extraProperties), MissingProperties::Extra}});
            return;
        }
    }

    // Reminder: left is the supertype, right is the subtype.
    // Width subtyping: any property in the supertype must be in the subtype,
    // and the types must agree.
    for (const auto& [name, prop] : lt->props)
    {
        const auto& r = rt->props.find(name);
        if (r != rt->props.end())
        {
            // TODO: read-only properties don't need invariance
            Resetter resetter{&variance};
            variance = Invariant;

            Unifier innerState = makeChildUnifier();
            innerState.tryUnify_(prop.type, r->second.type);

            if (FFlag::LuauExtendedTypeMismatchError)
                checkChildUnifierTypeMismatch(innerState.errors, name, left, right);
            else
                checkChildUnifierTypeMismatch(innerState.errors, left, right);

            if (innerState.errors.empty())
                log.concat(std::move(innerState.log));
            else
                innerState.log.rollback();
        }
        else if (rt->indexer && isString(rt->indexer->indexType))
        {
            // TODO: read-only indexers don't need invariance
            // TODO: really we should only allow this if prop.type is optional.
            Resetter resetter{&variance};
            variance = Invariant;

            Unifier innerState = makeChildUnifier();
            innerState.tryUnify_(prop.type, rt->indexer->indexResultType);

            if (FFlag::LuauExtendedTypeMismatchError)
                checkChildUnifierTypeMismatch(innerState.errors, name, left, right);
            else
                checkChildUnifierTypeMismatch(innerState.errors, left, right);

            if (innerState.errors.empty())
                log.concat(std::move(innerState.log));
            else
                innerState.log.rollback();
        }
        else if (isOptional(prop.type) || get<AnyTypeVar>(follow(prop.type)))
        // TODO: this case is unsound, but without it our test suite fails. CLI-46031
        // TODO: should isOptional(anyType) be true?
        {
        }
        else if (rt->state == TableState::Free)
        {
            log(rt);
            rt->props[name] = prop;
        }
        else
            missingProperties.push_back(name);
    }

    for (const auto& [name, prop] : rt->props)
    {
        if (lt->props.count(name))
        {
            // If both lt and rt contain the property, then
            // we're done since we already unified them above
        }
        else if (lt->indexer && isString(lt->indexer->indexType))
        {
            // TODO: read-only indexers don't need invariance
            // TODO: really we should only allow this if prop.type is optional.
            Resetter resetter{&variance};
            variance = Invariant;

            Unifier innerState = makeChildUnifier();
            innerState.tryUnify_(prop.type, lt->indexer->indexResultType);

            if (FFlag::LuauExtendedTypeMismatchError)
                checkChildUnifierTypeMismatch(innerState.errors, name, left, right);
            else
                checkChildUnifierTypeMismatch(innerState.errors, left, right);

            if (innerState.errors.empty())
                log.concat(std::move(innerState.log));
            else
                innerState.log.rollback();
        }
        else if (lt->state == TableState::Unsealed)
        {
            // TODO: this case is unsound when variance is Invariant, but without it lua-apps fails to typecheck.
            // TODO: file a JIRA
            // TODO: hopefully readonly/writeonly properties will fix this.
            Property clone = prop;
            clone.type = deeplyOptional(clone.type);
            log(left);
            lt->props[name] = clone;
        }
        else if (variance == Covariant)
        {
        }
        else if (isOptional(prop.type) || get<AnyTypeVar>(follow(prop.type)))
        // TODO: this case is unsound, but without it our test suite fails. CLI-46031
        // TODO: should isOptional(anyType) be true?
        {
        }
        else if (lt->state == TableState::Free)
        {
            log(left);
            lt->props[name] = prop;
        }
        else
            extraProperties.push_back(name);
    }

    // Unify indexers
    if (lt->indexer && rt->indexer)
    {
        // TODO: read-only indexers don't need invariance
        Resetter resetter{&variance};
        variance = Invariant;

        Unifier innerState = makeChildUnifier();
        innerState.tryUnify(*lt->indexer, *rt->indexer);
        checkChildUnifierTypeMismatch(innerState.errors, left, right);
        if (innerState.errors.empty())
            log.concat(std::move(innerState.log));
        else
            innerState.log.rollback();
    }
    else if (lt->indexer)
    {
        if (rt->state == TableState::Unsealed || rt->state == TableState::Free)
        {
            // passing/assigning a table without an indexer to something that has one
            // e.g. table.insert(t, 1) where t is a non-sealed table and doesn't have an indexer.
            // TODO: we only need to do this if the supertype's indexer is read/write
            // since that can add indexed elements.
            log(right);
            rt->indexer = lt->indexer;
        }
    }
    else if (rt->indexer && variance == Invariant)
    {
        // Symmetric if we are invariant
        if (lt->state == TableState::Unsealed || lt->state == TableState::Free)
        {
            log(left);
            lt->indexer = rt->indexer;
        }
    }

    if (!missingProperties.empty())
    {
        errors.push_back(TypeError{location, MissingProperties{left, right, std::move(missingProperties)}});
        return;
    }

    if (!extraProperties.empty())
    {
        errors.push_back(TypeError{location, MissingProperties{left, right, std::move(extraProperties), MissingProperties::Extra}});
        return;
    }

    /*
     * TypeVars are commonly cyclic, so it is entirely possible
     * for unifying a property of a table to change the table itself!
     * We need to check for this and start over if we notice this occurring.
     *
     * I believe this is guaranteed to terminate eventually because this will
     * only happen when a free table is bound to another table.
     */
    if (lt->boundTo || rt->boundTo)
        return tryUnify_(left, right);

    if (lt->state == TableState::Free)
    {
        log(lt);
        lt->boundTo = right;
    }
    else if (rt->state == TableState::Free)
    {
        log(rt);
        rt->boundTo = left;
    }
}

TypeId Unifier::deeplyOptional(TypeId ty, std::unordered_map<TypeId, TypeId> seen)
{
    ty = follow(ty);
    if (get<AnyTypeVar>(ty))
        return ty;
    else if (isOptional(ty))
        return ty;
    else if (const TableTypeVar* ttv = get<TableTypeVar>(ty))
    {
        TypeId& result = seen[ty];
        if (result)
            return result;
        result = types->addType(*ttv);
        TableTypeVar* resultTtv = getMutable<TableTypeVar>(result);
        for (auto& [name, prop] : resultTtv->props)
            prop.type = deeplyOptional(prop.type, seen);
        return types->addType(UnionTypeVar{{getSingletonTypes().nilType, result}});
    }
    else
        return types->addType(UnionTypeVar{{getSingletonTypes().nilType, ty}});
}

void Unifier::DEPRECATED_tryUnifyTables(TypeId left, TypeId right, bool isIntersection)
{
    LUAU_ASSERT(!FFlag::LuauTableSubtypingVariance2);
    Resetter resetter{&variance};
    variance = Invariant;

    TableTypeVar* lt = getMutable<TableTypeVar>(left);
    TableTypeVar* rt = getMutable<TableTypeVar>(right);
    if (!lt || !rt)
        ice("passed non-table types to unifyTables");

    if (lt->state == TableState::Sealed && rt->state == TableState::Sealed)
        return tryUnifySealedTables(left, right, isIntersection);
    else if ((lt->state == TableState::Sealed && rt->state == TableState::Unsealed) ||
             (lt->state == TableState::Unsealed && rt->state == TableState::Sealed))
        return tryUnifySealedTables(left, right, isIntersection);
    else if ((lt->state == TableState::Sealed && rt->state == TableState::Generic) ||
             (lt->state == TableState::Generic && rt->state == TableState::Sealed))
        errors.push_back(TypeError{location, TypeMismatch{left, right}});
    else if ((lt->state == TableState::Free) != (rt->state == TableState::Free)) // one table is free and the other is not
    {
        TypeId freeTypeId = rt->state == TableState::Free ? right : left;
        TypeId otherTypeId = rt->state == TableState::Free ? left : right;

        return tryUnifyFreeTable(freeTypeId, otherTypeId);
    }
    else if (lt->state == TableState::Free && rt->state == TableState::Free)
    {
        tryUnifyFreeTable(left, right);

        // avoid creating a cycle when the types are already pointing at each other
        if (follow(left) != follow(right))
        {
            log(lt);
            lt->boundTo = right;
        }
        return;
    }
    else if (lt->state != TableState::Sealed && rt->state != TableState::Sealed)
    {
        // All free tables are checked in one of the branches above
        LUAU_ASSERT(lt->state != TableState::Free);
        LUAU_ASSERT(rt->state != TableState::Free);

        // Tables must have exactly the same props and their types must all unify
        // I honestly have no idea if this is remotely close to reasonable.
        for (const auto& [name, prop] : lt->props)
        {
            const auto& r = rt->props.find(name);
            if (r == rt->props.end())
                errors.push_back(TypeError{location, UnknownProperty{right, name}});
            else
                tryUnify_(prop.type, r->second.type);
        }

        if (lt->indexer && rt->indexer)
            tryUnify(*lt->indexer, *rt->indexer);
        else if (lt->indexer)
        {
            // passing/assigning a table without an indexer to something that has one
            // e.g. table.insert(t, 1) where t is a non-sealed table and doesn't have an indexer.
            if (rt->state == TableState::Unsealed)
                rt->indexer = lt->indexer;
            else
                errors.push_back(TypeError{location, CannotExtendTable{right, CannotExtendTable::Indexer}});
        }
    }
    else if (lt->state == TableState::Sealed)
    {
        // lt is sealed and so it must be possible for rt to have precisely the same shape
        // Verify that this is the case, then bind rt to lt.
        ice("unsealed tables are not working yet", location);
    }
    else if (rt->state == TableState::Sealed)
        return tryUnifyTables(right, left, isIntersection);
    else
        ice("tryUnifyTables");
}

void Unifier::tryUnifyFreeTable(TypeId freeTypeId, TypeId otherTypeId)
{
    TableTypeVar* freeTable = getMutable<TableTypeVar>(freeTypeId);
    TableTypeVar* otherTable = getMutable<TableTypeVar>(otherTypeId);
    if (!freeTable || !otherTable)
        ice("passed non-table types to tryUnifyFreeTable");

    // Any properties in freeTable must unify with those in otherTable.
    // Then bind freeTable to otherTable.
    for (const auto& [freeName, freeProp] : freeTable->props)
    {
        if (auto otherProp = findTablePropertyRespectingMeta(otherTypeId, freeName))
        {
            tryUnify_(*otherProp, freeProp.type);

            /*
             * TypeVars are commonly cyclic, so it is entirely possible
             * for unifying a property of a table to change the table itself!
             * We need to check for this and start over if we notice this occurring.
             *
             * I believe this is guaranteed to terminate eventually because this will
             * only happen when a free table is bound to another table.
             */
            if (!get<TableTypeVar>(freeTypeId) || !get<TableTypeVar>(otherTypeId))
                return tryUnify_(freeTypeId, otherTypeId);

            if (freeTable->boundTo)
                return tryUnify_(freeTypeId, otherTypeId);
        }
        else
        {
            // If the other table is also free, then we are learning that it has more
            // properties than we previously thought.  Else, it is an error.
            if (otherTable->state == TableState::Free)
                otherTable->props.insert({freeName, freeProp});
            else
                errors.push_back(TypeError{location, UnknownProperty{otherTypeId, freeName}});
        }
    }

    if (freeTable->indexer && otherTable->indexer)
    {
        Unifier innerState = makeChildUnifier();
        innerState.tryUnify(*freeTable->indexer, *otherTable->indexer);

        checkChildUnifierTypeMismatch(innerState.errors, freeTypeId, otherTypeId);

        log.concat(std::move(innerState.log));
    }
    else if (otherTable->state == TableState::Free && freeTable->indexer)
        freeTable->indexer = otherTable->indexer;

    if (!freeTable->boundTo && otherTable->state != TableState::Free)
    {
        log(freeTable);
        freeTable->boundTo = otherTypeId;
    }
}

void Unifier::tryUnifySealedTables(TypeId left, TypeId right, bool isIntersection)
{
    TableTypeVar* lt = getMutable<TableTypeVar>(left);
    TableTypeVar* rt = getMutable<TableTypeVar>(right);
    if (!lt || !rt)
        ice("passed non-table types to unifySealedTables");

    Unifier innerState = makeChildUnifier();

    std::vector<std::string> missingPropertiesInSuper;
    bool isUnnamedTable = rt->name == std::nullopt && rt->syntheticName == std::nullopt;
    bool errorReported = false;

    // Optimization: First test that the property sets are compatible without doing any recursive unification
    if (FFlag::LuauTableUnificationEarlyTest && !rt->indexer)
    {
        for (const auto& [propName, superProp] : lt->props)
        {
            auto subIter = rt->props.find(propName);
            if (subIter == rt->props.end() && !isOptional(superProp.type))
                missingPropertiesInSuper.push_back(propName);
        }

        if (!missingPropertiesInSuper.empty())
        {
            errors.push_back(TypeError{location, MissingProperties{left, right, std::move(missingPropertiesInSuper)}});
            return;
        }
    }

    // Tables must have exactly the same props and their types must all unify
    for (const auto& it : lt->props)
    {
        const auto& r = rt->props.find(it.first);
        if (r == rt->props.end())
        {
            if (isOptional(it.second.type))
                continue;

            missingPropertiesInSuper.push_back(it.first);

            innerState.errors.push_back(TypeError{location, TypeMismatch{left, right}});
        }
        else
        {
            if (isUnnamedTable && r->second.location)
            {
                size_t oldErrorSize = innerState.errors.size();
                Location old = innerState.location;
                innerState.location = *r->second.location;
                innerState.tryUnify_(it.second.type, r->second.type);
                innerState.location = old;

                if (oldErrorSize != innerState.errors.size() && !errorReported)
                {
                    errorReported = true;
                    errors.push_back(innerState.errors.back());
                }
            }
            else
            {
                innerState.tryUnify_(it.second.type, r->second.type);
            }
        }
    }

    if (lt->indexer || rt->indexer)
    {
        if (lt->indexer && rt->indexer)
            innerState.tryUnify(*lt->indexer, *rt->indexer);
        else if (rt->state == TableState::Unsealed)
        {
            if (lt->indexer && !rt->indexer)
                rt->indexer = lt->indexer;
        }
        else if (lt->state == TableState::Unsealed)
        {
            if (rt->indexer && !lt->indexer)
                lt->indexer = rt->indexer;
        }
        else if (lt->indexer)
        {
            innerState.tryUnify_(lt->indexer->indexType, getSingletonTypes().stringType);
            // We already try to unify properties in both tables.
            // Skip those and just look for the ones remaining and see if they fit into the indexer.
            for (const auto& [name, type] : rt->props)
            {
                const auto& it = lt->props.find(name);
                if (it == lt->props.end())
                    innerState.tryUnify_(lt->indexer->indexResultType, type.type);
            }
        }
        else
            innerState.errors.push_back(TypeError{location, TypeMismatch{left, right}});
    }

    log.concat(std::move(innerState.log));

    if (errorReported)
        return;

    if (!missingPropertiesInSuper.empty())
    {
        errors.push_back(TypeError{location, MissingProperties{left, right, std::move(missingPropertiesInSuper)}});
        return;
    }

    // If the superTy/left is an immediate part of an intersection type, do not do extra-property check.
    // Otherwise, we would falsely generate an extra-property-error for 's' in this code:
    // local a: {n: number} & {s: string} = {n=1, s=""}
    // When checking against the table '{n: number}'.
    if (!isIntersection && lt->state != TableState::Unsealed && !lt->indexer)
    {
        // Check for extra properties in the subTy
        std::vector<std::string> extraPropertiesInSub;

        for (const auto& it : rt->props)
        {
            const auto& r = lt->props.find(it.first);
            if (r == lt->props.end())
            {
                if (isOptional(it.second.type))
                    continue;

                extraPropertiesInSub.push_back(it.first);
            }
        }

        if (!extraPropertiesInSub.empty())
        {
            errors.push_back(TypeError{location, MissingProperties{left, right, std::move(extraPropertiesInSub), MissingProperties::Extra}});
            return;
        }
    }

    checkChildUnifierTypeMismatch(innerState.errors, left, right);
}

void Unifier::tryUnifyWithMetatable(TypeId metatable, TypeId other, bool reversed)
{
    const MetatableTypeVar* lhs = get<MetatableTypeVar>(metatable);
    if (!lhs)
        ice("tryUnifyMetatable invoked with non-metatable TypeVar");

    TypeError mismatchError = TypeError{location, TypeMismatch{reversed ? other : metatable, reversed ? metatable : other}};

    if (const MetatableTypeVar* rhs = get<MetatableTypeVar>(other))
    {
        Unifier innerState = makeChildUnifier();
        innerState.tryUnify_(lhs->table, rhs->table);
        innerState.tryUnify_(lhs->metatable, rhs->metatable);

        if (FFlag::LuauExtendedTypeMismatchError)
        {
            if (auto e = hasUnificationTooComplex(innerState.errors))
                errors.push_back(*e);
            else if (!innerState.errors.empty())
                errors.push_back(
                    TypeError{location, TypeMismatch{reversed ? other : metatable, reversed ? metatable : other, "", innerState.errors.front()}});
        }
        else
        {
            checkChildUnifierTypeMismatch(innerState.errors, reversed ? other : metatable, reversed ? metatable : other);
        }

        log.concat(std::move(innerState.log));
    }
    else if (TableTypeVar* rhs = getMutable<TableTypeVar>(other))
    {
        switch (rhs->state)
        {
        case TableState::Free:
        {
            tryUnify_(lhs->table, other);
            rhs->boundTo = metatable;

            break;
        }
        // We know the shape of sealed, unsealed, and generic tables; you can't add a metatable on to any of these.
        case TableState::Sealed:
        case TableState::Unsealed:
        case TableState::Generic:
            errors.push_back(mismatchError);
        }
    }
    else if (get<AnyTypeVar>(other) || get<ErrorTypeVar>(other))
    {
    }
    else
    {
        errors.push_back(mismatchError);
    }
}

// Class unification is almost, but not quite symmetrical.  We use the 'reversed' boolean to indicate which scenario we are evaluating.
void Unifier::tryUnifyWithClass(TypeId superTy, TypeId subTy, bool reversed)
{
    if (reversed)
        std::swap(superTy, subTy);

    auto fail = [&]() {
        if (!reversed)
            errors.push_back(TypeError{location, TypeMismatch{superTy, subTy}});
        else
            errors.push_back(TypeError{location, TypeMismatch{subTy, superTy}});
    };

    const ClassTypeVar* superClass = get<ClassTypeVar>(superTy);
    if (!superClass)
        ice("tryUnifyClass invoked with non-class TypeVar");

    if (const ClassTypeVar* subClass = get<ClassTypeVar>(subTy))
    {
        switch (variance)
        {
        case Covariant:
            if (!isSubclass(subClass, superClass))
                return fail();
            return;
        case Invariant:
            if (subClass != superClass)
                return fail();
            return;
        }
        ice("Illegal variance setting!");
    }
    else if (TableTypeVar* table = getMutable<TableTypeVar>(subTy))
    {
        /**
         * A free table is something whose shape we do not exactly know yet.
         * Thus, it is entirely reasonable that we might discover that it is being used as some class type.
         * In this case, the free table must indeed be that exact class.
         * For this to hold, the table must not have any properties that the class does not.
         * Further, all properties of the table should unify cleanly with the matching class properties.
         * TODO: What does it mean for the table to have an indexer? (probably failure?)
         *
         * Tables that are not free are known to be actual tables.
         */
        if (table->state != TableState::Free)
            return fail();

        bool ok = true;

        for (const auto& [propName, prop] : table->props)
        {
            const Property* classProp = lookupClassProp(superClass, propName);
            if (!classProp)
            {
                ok = false;
                errors.push_back(TypeError{location, UnknownProperty{superTy, propName}});
                if (!FFlag::LuauExtendedClassMismatchError)
                    tryUnify_(prop.type, getSingletonTypes().errorRecoveryType());
            }
            else
            {
                if (FFlag::LuauExtendedClassMismatchError)
                {
                    Unifier innerState = makeChildUnifier();
                    innerState.tryUnify_(prop.type, classProp->type);

                    checkChildUnifierTypeMismatch(innerState.errors, propName, reversed ? subTy : superTy, reversed ? superTy : subTy);

                    if (innerState.errors.empty())
                    {
                        log.concat(std::move(innerState.log));
                    }
                    else
                    {
                        ok = false;
                        innerState.log.rollback();
                    }
                }
                else
                {
                    tryUnify_(prop.type, classProp->type);
                }
            }
        }

        if (table->indexer)
        {
            ok = false;
            std::string msg = "Class " + superClass->name + " does not have an indexer";
            errors.push_back(TypeError{location, GenericError{msg}});
        }

        if (!ok)
            return;

        log(table);
        table->boundTo = superTy;
    }
    else
        return fail();
}

void Unifier::tryUnify(const TableIndexer& superIndexer, const TableIndexer& subIndexer)
{
    tryUnify_(superIndexer.indexType, subIndexer.indexType);
    tryUnify_(superIndexer.indexResultType, subIndexer.indexResultType);
}

static void queueTypePack(std::vector<TypeId>& queue, DenseHashSet<TypePackId>& seenTypePacks, Unifier& state, TypePackId a, TypePackId anyTypePack)
{
    while (true)
    {
        a = follow(a);

        if (seenTypePacks.find(a))
            break;
        seenTypePacks.insert(a);

        if (get<Unifiable::Free>(a))
        {
            state.log(a);
            *asMutable(a) = Unifiable::Bound{anyTypePack};
        }
        else if (auto tp = get<TypePack>(a))
        {
            queue.insert(queue.end(), tp->head.begin(), tp->head.end());
            if (tp->tail)
                a = *tp->tail;
            else
                break;
        }
    }
}

void Unifier::tryUnifyVariadics(TypePackId superTp, TypePackId subTp, bool reversed, int subOffset)
{
    const VariadicTypePack* lv = get<VariadicTypePack>(superTp);
    if (!lv)
        ice("passed non-variadic pack to tryUnifyVariadics");

    if (const VariadicTypePack* rv = get<VariadicTypePack>(subTp))
        tryUnify_(reversed ? rv->ty : lv->ty, reversed ? lv->ty : rv->ty);
    else if (get<TypePack>(subTp))
    {
        TypePackIterator rIter = begin(subTp);
        TypePackIterator rEnd = end(subTp);

        std::advance(rIter, subOffset);

        while (rIter != rEnd)
        {
            tryUnify_(reversed ? *rIter : lv->ty, reversed ? lv->ty : *rIter);
            ++rIter;
        }

        if (std::optional<TypePackId> maybeTail = rIter.tail())
        {
            TypePackId tail = follow(*maybeTail);
            if (get<FreeTypePack>(tail))
            {
                log(tail);
                *asMutable(tail) = BoundTypePack{superTp};
            }
            else if (const VariadicTypePack* vtp = get<VariadicTypePack>(tail))
            {
                tryUnify_(lv->ty, vtp->ty);
            }
            else if (get<Unifiable::Generic>(tail))
            {
                errors.push_back(TypeError{location, GenericError{"Cannot unify variadic and generic packs"}});
            }
            else if (get<Unifiable::Error>(tail))
            {
                // Nothing to do here.
            }
            else
            {
                ice("Unknown TypePack kind");
            }
        }
    }
    else
    {
        errors.push_back(TypeError{location, GenericError{"Failed to unify variadic packs"}});
    }
}

static void tryUnifyWithAny(std::vector<TypeId>& queue, Unifier& state, DenseHashSet<TypeId>& seen, DenseHashSet<TypePackId>& seenTypePacks,
    TypeId anyType, TypePackId anyTypePack)
{
    while (!queue.empty())
    {
        TypeId ty = follow(queue.back());
        queue.pop_back();
        if (seen.find(ty))
            continue;
        seen.insert(ty);

        if (get<FreeTypeVar>(ty))
        {
            state.log(ty);
            *asMutable(ty) = BoundTypeVar{anyType};
        }
        else if (auto fun = get<FunctionTypeVar>(ty))
        {
            queueTypePack(queue, seenTypePacks, state, fun->argTypes, anyTypePack);
            queueTypePack(queue, seenTypePacks, state, fun->retType, anyTypePack);
        }
        else if (auto table = get<TableTypeVar>(ty))
        {
            for (const auto& [_name, prop] : table->props)
                queue.push_back(prop.type);

            if (table->indexer)
            {
                queue.push_back(table->indexer->indexType);
                queue.push_back(table->indexer->indexResultType);
            }
        }
        else if (auto mt = get<MetatableTypeVar>(ty))
        {
            queue.push_back(mt->table);
            queue.push_back(mt->metatable);
        }
        else if (get<ClassTypeVar>(ty))
        {
            // ClassTypeVars never contain free typevars.
        }
        else if (auto union_ = get<UnionTypeVar>(ty))
            queue.insert(queue.end(), union_->options.begin(), union_->options.end());
        else if (auto intersection = get<IntersectionTypeVar>(ty))
            queue.insert(queue.end(), intersection->parts.begin(), intersection->parts.end());
        else
        {
        } // Primitives, any, errors, and generics are left untouched.
    }
}

void Unifier::tryUnifyWithAny(TypeId any, TypeId ty)
{
    LUAU_ASSERT(get<AnyTypeVar>(any) || get<ErrorTypeVar>(any));

    // These types are not visited in general loop below
    if (get<PrimitiveTypeVar>(ty) || get<AnyTypeVar>(ty) || get<ClassTypeVar>(ty))
        return;

    const TypePackId anyTypePack = types->addTypePack(TypePackVar{VariadicTypePack{getSingletonTypes().anyType}});

    const TypePackId anyTP = get<AnyTypeVar>(any) ? anyTypePack : types->addTypePack(TypePackVar{Unifiable::Error{}});

    std::vector<TypeId> queue = {ty};

    sharedState.tempSeenTy.clear();
    sharedState.tempSeenTp.clear();

    Luau::tryUnifyWithAny(queue, *this, sharedState.tempSeenTy, sharedState.tempSeenTp, getSingletonTypes().anyType, anyTP);
}

void Unifier::tryUnifyWithAny(TypePackId any, TypePackId ty)
{
    LUAU_ASSERT(get<Unifiable::Error>(any));

    const TypeId anyTy = getSingletonTypes().errorRecoveryType();

    std::vector<TypeId> queue;

    sharedState.tempSeenTy.clear();
    sharedState.tempSeenTp.clear();

    queueTypePack(queue, sharedState.tempSeenTp, *this, ty, any);

    Luau::tryUnifyWithAny(queue, *this, sharedState.tempSeenTy, sharedState.tempSeenTp, anyTy, any);
}

std::optional<TypeId> Unifier::findTablePropertyRespectingMeta(TypeId lhsType, Name name)
{
    return Luau::findTablePropertyRespectingMeta(errors, globalScope, lhsType, name, location);
}

void Unifier::occursCheck(TypeId needle, TypeId haystack)
{
    sharedState.tempSeenTy.clear();

    return occursCheck(sharedState.tempSeenTy, needle, haystack);
}

void Unifier::occursCheck(DenseHashSet<TypeId>& seen, TypeId needle, TypeId haystack)
{
    RecursionLimiter _ra(&sharedState.counters.recursionCount, FInt::LuauTypeInferRecursionLimit);

    needle = follow(needle);
    haystack = follow(haystack);

    if (seen.find(haystack))
        return;

    seen.insert(haystack);

    if (get<Unifiable::Error>(needle))
        return;

    if (!get<Unifiable::Free>(needle))
        ice("Expected needle to be free");

    if (needle == haystack)
    {
        errors.push_back(TypeError{location, OccursCheckFailed{}});
        log(needle);
        *asMutable(needle) = *getSingletonTypes().errorRecoveryType();
        return;
    }

    auto check = [&](TypeId tv) {
        occursCheck(seen, needle, tv);
    };

    if (get<FreeTypeVar>(haystack))
        return;
    else if (auto a = get<FunctionTypeVar>(haystack))
    {
        if (!FFlag::LuauOccursCheckOkWithRecursiveFunctions)
        {
            for (TypeId ty : a->argTypes)
                check(ty);

            for (TypeId ty : a->retType)
                check(ty);
        }
    }
    else if (auto a = get<UnionTypeVar>(haystack))
    {
        for (TypeId ty : a->options)
            check(ty);
    }
    else if (auto a = get<IntersectionTypeVar>(haystack))
    {
        for (TypeId ty : a->parts)
            check(ty);
    }
}

void Unifier::occursCheck(TypePackId needle, TypePackId haystack)
{
    sharedState.tempSeenTp.clear();

    return occursCheck(sharedState.tempSeenTp, needle, haystack);
}

void Unifier::occursCheck(DenseHashSet<TypePackId>& seen, TypePackId needle, TypePackId haystack)
{
    needle = follow(needle);
    haystack = follow(haystack);

    if (seen.find(haystack))
        return;

    seen.insert(haystack);

    if (get<Unifiable::Error>(needle))
        return;

    if (!get<Unifiable::Free>(needle))
        ice("Expected needle pack to be free");

    RecursionLimiter _ra(&sharedState.counters.recursionCount, FInt::LuauTypeInferRecursionLimit);

    while (!get<ErrorTypeVar>(haystack))
    {
        if (needle == haystack)
        {
            errors.push_back(TypeError{location, OccursCheckFailed{}});
            log(needle);
            *asMutable(needle) = *getSingletonTypes().errorRecoveryTypePack();
            return;
        }

        if (auto a = get<TypePack>(haystack))
        {
            if (!FFlag::LuauOccursCheckOkWithRecursiveFunctions)
            {
                for (const auto& ty : a->head)
                {
                    if (auto f = get<FunctionTypeVar>(follow(ty)))
                    {
                        occursCheck(seen, needle, f->argTypes);
                        occursCheck(seen, needle, f->retType);
                    }
                }
            }

            if (a->tail)
            {
                haystack = follow(*a->tail);
                continue;
            }
        }
        break;
    }
}

Unifier Unifier::makeChildUnifier()
{
    return Unifier{types, mode, globalScope, log.sharedSeen, location, variance, sharedState};
}

bool Unifier::isNonstrictMode() const
{
    return (mode == Mode::Nonstrict) || (mode == Mode::NoCheck);
}

void Unifier::checkChildUnifierTypeMismatch(const ErrorVec& innerErrors, TypeId wantedType, TypeId givenType)
{
    if (auto e = hasUnificationTooComplex(innerErrors))
        errors.push_back(*e);
    else if (!innerErrors.empty())
        errors.push_back(TypeError{location, TypeMismatch{wantedType, givenType}});
}

void Unifier::checkChildUnifierTypeMismatch(const ErrorVec& innerErrors, const std::string& prop, TypeId wantedType, TypeId givenType)
{
    LUAU_ASSERT(FFlag::LuauExtendedTypeMismatchError || FFlag::LuauExtendedClassMismatchError);

    if (auto e = hasUnificationTooComplex(innerErrors))
        errors.push_back(*e);
    else if (!innerErrors.empty())
        errors.push_back(
            TypeError{location, TypeMismatch{wantedType, givenType, format("Property '%s' is not compatible.", prop.c_str()), innerErrors.front()}});
}

void Unifier::ice(const std::string& message, const Location& location)
{
    sharedState.iceHandler->ice(message, location);
}

void Unifier::ice(const std::string& message)
{
    sharedState.iceHandler->ice(message);
}

} // namespace Luau
