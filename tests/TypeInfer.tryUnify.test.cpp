// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/Parser.h"
#include "Luau/Scope.h"
#include "Luau/TypeInfer.h"
#include "Luau/TypeVar.h"

#include "Fixture.h"

#include "doctest.h"

LUAU_FASTFLAG(LuauQuantifyInPlace2);

using namespace Luau;

struct TryUnifyFixture : Fixture
{
    TypeArena arena;
    ScopePtr globalScope{new Scope{arena.addTypePack({TypeId{}})}};
    InternalErrorReporter iceHandler;
    UnifierSharedState unifierState{&iceHandler};
    Unifier state{&arena, Mode::Strict, globalScope, Location{}, Variance::Covariant, unifierState};
};

TEST_SUITE_BEGIN("TryUnifyTests");

TEST_CASE_FIXTURE(TryUnifyFixture, "primitives_unify")
{
    TypeVar numberOne{TypeVariant{PrimitiveTypeVar{PrimitiveTypeVar::Number}}};
    TypeVar numberTwo = numberOne;

    state.tryUnify(&numberOne, &numberTwo);

    CHECK(state.errors.empty());
}

TEST_CASE_FIXTURE(TryUnifyFixture, "compatible_functions_are_unified")
{
    TypeVar functionOne{
        TypeVariant{FunctionTypeVar(arena.addTypePack({arena.freshType(globalScope->level)}), arena.addTypePack({typeChecker.numberType}))}};

    TypeVar functionTwo{TypeVariant{
        FunctionTypeVar(arena.addTypePack({arena.freshType(globalScope->level)}), arena.addTypePack({arena.freshType(globalScope->level)}))}};

    state.tryUnify(&functionOne, &functionTwo);
    CHECK(state.errors.empty());

    CHECK_EQ(functionOne, functionTwo);
}

TEST_CASE_FIXTURE(TryUnifyFixture, "incompatible_functions_are_preserved")
{
    TypePackVar argPackOne{TypePack{{arena.freshType(globalScope->level)}, std::nullopt}};
    TypeVar functionOne{
        TypeVariant{FunctionTypeVar(arena.addTypePack({arena.freshType(globalScope->level)}), arena.addTypePack({typeChecker.numberType}))}};

    TypeVar functionOneSaved = functionOne;

    TypePackVar argPackTwo{TypePack{{arena.freshType(globalScope->level)}, std::nullopt}};
    TypeVar functionTwo{
        TypeVariant{FunctionTypeVar(arena.addTypePack({arena.freshType(globalScope->level)}), arena.addTypePack({typeChecker.stringType}))}};

    TypeVar functionTwoSaved = functionTwo;

    state.tryUnify(&functionOne, &functionTwo);
    CHECK(!state.errors.empty());

    CHECK_EQ(functionOne, functionOneSaved);
    CHECK_EQ(functionTwo, functionTwoSaved);
}

TEST_CASE_FIXTURE(TryUnifyFixture, "tables_can_be_unified")
{
    TypeVar tableOne{TypeVariant{
        TableTypeVar{{{"foo", {arena.freshType(globalScope->level)}}}, std::nullopt, globalScope->level, TableState::Unsealed},
    }};

    TypeVar tableTwo{TypeVariant{
        TableTypeVar{{{"foo", {arena.freshType(globalScope->level)}}}, std::nullopt, globalScope->level, TableState::Unsealed},
    }};

    CHECK_NE(*getMutable<TableTypeVar>(&tableOne)->props["foo"].type, *getMutable<TableTypeVar>(&tableTwo)->props["foo"].type);

    state.tryUnify(&tableOne, &tableTwo);

    CHECK(state.errors.empty());

    CHECK_EQ(*getMutable<TableTypeVar>(&tableOne)->props["foo"].type, *getMutable<TableTypeVar>(&tableTwo)->props["foo"].type);
}

TEST_CASE_FIXTURE(TryUnifyFixture, "incompatible_tables_are_preserved")
{
    TypeVar tableOne{TypeVariant{
        TableTypeVar{{{"foo", {arena.freshType(globalScope->level)}}, {"bar", {typeChecker.numberType}}}, std::nullopt, globalScope->level,
            TableState::Unsealed},
    }};

    TypeVar tableTwo{TypeVariant{
        TableTypeVar{{{"foo", {arena.freshType(globalScope->level)}}, {"bar", {typeChecker.stringType}}}, std::nullopt, globalScope->level,
            TableState::Unsealed},
    }};

    CHECK_NE(*getMutable<TableTypeVar>(&tableOne)->props["foo"].type, *getMutable<TableTypeVar>(&tableTwo)->props["foo"].type);

    state.tryUnify(&tableOne, &tableTwo);

    CHECK_EQ(1, state.errors.size());

    state.log.rollback();

    CHECK_NE(*getMutable<TableTypeVar>(&tableOne)->props["foo"].type, *getMutable<TableTypeVar>(&tableTwo)->props["foo"].type);
}

TEST_CASE_FIXTURE(TryUnifyFixture, "members_of_failed_typepack_unification_are_unified_with_errorType")
{
    CheckResult result = check(R"(
        function f(arg: number) end
        local a
        local b
        f(a, b)
    )");

    LUAU_REQUIRE_ERROR_COUNT(1, result);

    CHECK_EQ("a", toString(requireType("a")));
    CHECK_EQ("*unknown*", toString(requireType("b")));
}

TEST_CASE_FIXTURE(TryUnifyFixture, "result_of_failed_typepack_unification_is_constrained")
{
    ScopedFastFlag sff{"LuauErrorRecoveryType", true};

    CheckResult result = check(R"(
        function f(arg: number) return arg end
        local a
        local b
        local c = f(a, b)
    )");

    LUAU_REQUIRE_ERROR_COUNT(1, result);

    CHECK_EQ("a", toString(requireType("a")));
    CHECK_EQ("*unknown*", toString(requireType("b")));
    CHECK_EQ("number", toString(requireType("c")));
}

TEST_CASE_FIXTURE(TryUnifyFixture, "typepack_unification_should_trim_free_tails")
{
    CheckResult result = check(R"(
        --!strict
        local function f(v: number)
            if v % 2 == 0 then
                return true
            end
        end

        return function()
            return (f(1))
        end
    )");

    LUAU_REQUIRE_ERROR_COUNT(1, result);
    if (FFlag::LuauQuantifyInPlace2)
        CHECK_EQ("(number) -> boolean", toString(requireType("f")));
    else
        CHECK_EQ("(number) -> (boolean)", toString(requireType("f")));
}

TEST_CASE_FIXTURE(TryUnifyFixture, "variadic_type_pack_unification")
{
    TypePackVar testPack{TypePack{{typeChecker.numberType, typeChecker.stringType}, std::nullopt}};
    TypePackVar variadicPack{VariadicTypePack{typeChecker.numberType}};

    state.tryUnify(&variadicPack, &testPack);
    CHECK(!state.errors.empty());
}

TEST_CASE_FIXTURE(TryUnifyFixture, "variadic_tails_respect_progress")
{
    TypePackVar variadicPack{VariadicTypePack{typeChecker.booleanType}};
    TypePackVar a{TypePack{{typeChecker.numberType, typeChecker.stringType, typeChecker.booleanType, typeChecker.booleanType}}};
    TypePackVar b{TypePack{{typeChecker.numberType, typeChecker.stringType}, &variadicPack}};

    state.tryUnify(&a, &b);
    CHECK(state.errors.empty());
}

TEST_CASE_FIXTURE(TryUnifyFixture, "variadics_should_use_reversed_properly")
{
    CheckResult result = check(R"(
        --!strict
        local function f<T>(...: T): ...T
            return ...
        end

        local x: string = f(1)
    )");

    LUAU_REQUIRE_ERROR_COUNT(1, result);
    TypeMismatch* tm = get<TypeMismatch>(result.errors[0]);
    REQUIRE(tm);
    CHECK_EQ(toString(tm->givenType), "number");
    CHECK_EQ(toString(tm->wantedType), "string");
}

TEST_CASE_FIXTURE(TryUnifyFixture, "cli_41095_concat_log_in_sealed_table_unification")
{
    CheckResult result = check(R"(
        --!strict
        table.insert()
    )");

    LUAU_REQUIRE_ERROR_COUNT(2, result);
    CHECK_EQ(toString(result.errors[0]), "No overload for function accepts 0 arguments.");
    CHECK_EQ(toString(result.errors[1]), "Available overloads: ({a}, a) -> (); and ({a}, number, a) -> ()");
}

TEST_CASE_FIXTURE(TryUnifyFixture, "undo_new_prop_on_unsealed_table")
{
    ScopedFastFlag flags[] = {
        {"LuauTableSubtypingVariance2", true},
    };
    // I am not sure how to make this happen in Luau code.

    TypeId unsealedTable = arena.addType(TableTypeVar{TableState::Unsealed, TypeLevel{}});
    TypeId sealedTable = arena.addType(TableTypeVar{
        {{"prop", Property{getSingletonTypes().numberType}}},
        std::nullopt,
        TypeLevel{},
        TableState::Sealed
    });

    const TableTypeVar* ttv = get<TableTypeVar>(unsealedTable);
    REQUIRE(ttv);

    state.tryUnify(unsealedTable, sealedTable);

    // To be honest, it's really quite spooky here that we're amending an unsealed table in this case.
    CHECK(!ttv->props.empty());

    state.log.rollback();

    CHECK(ttv->props.empty());
}

TEST_SUITE_END();
