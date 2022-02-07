#include "kqp_opt_phy_effects_impl.h"

namespace NKikimr::NKqp::NOpt {

using namespace NYql;
using namespace NYql::NDq;
using namespace NYql::NNodes;

TExprNode::TPtr MakeMessage(TStringBuf message, TPositionHandle pos, TExprContext& ctx) {
    return ctx.NewCallable(pos, "Utf8", { ctx.NewAtom(pos, message) });
}

TMaybe<TCondenseInputResult> CondenseInput(const TExprBase& input, TExprContext& ctx) {
    TVector<TExprBase> stageInputs;
    TVector<TCoArgument> stageArguments;

    if (IsDqPureExpr(input)) {
        YQL_ENSURE(input.Ref().GetTypeAnn()->GetKind() == ETypeAnnotationKind::List, "" << input.Ref().Dump());
        auto stream = Build<TCoToStream>(ctx, input.Pos())
            .Input<TCoJust>()
                .Input(input)
                .Build()
            .Done();

        return TCondenseInputResult { .Stream = stream };
    }

    if (!input.Maybe<TDqCnUnionAll>()) {
        return {};
    }

    auto arg = Build<TCoArgument>(ctx, input.Pos())
        .Name("input_rows")
        .Done();

    stageInputs.push_back(input);
    stageArguments.push_back(arg);

    auto condense = Build<TCoCondense>(ctx, input.Pos())
        .Input(arg)
        .State<TCoList>()
            .ListType<TCoTypeOf>()
                .Value(input)
                .Build()
            .Build()
        .SwitchHandler()
            .Args({"item", "state"})
            .Body(MakeBool<false>(input.Pos(), ctx))
            .Build()
        .UpdateHandler()
            .Args({"item", "state"})
            .Body<TCoAppend>()
                .List("state")
                .Item("item")
                .Build()
            .Build()
        .Done();

    return TCondenseInputResult {
        .Stream = condense,
        .StageInputs = stageInputs,
        .StageArgs = stageArguments
    };
}

TMaybe<TCondenseInputResult> CondenseAndDeduplicateInput(const TExprBase& input, const TKikimrTableDescription& table,
    TExprContext& ctx)
{
    auto condenseResult = CondenseInput(input, ctx);
    if (!condenseResult) {
        return {};
    }

    auto listArg = TCoArgument(ctx.NewArgument(input.Pos(), "list_arg"));

    auto deduplicated = Build<TCoFlatMap>(ctx, input.Pos())
        .Input(condenseResult->Stream)
        .Lambda()
            .Args({listArg})
            .Body<TCoJust>()
                .Input(RemoveDuplicateKeyFromInput(listArg, table, input.Pos(), ctx))
                .Build()
            .Build()
        .Done();

    return TCondenseInputResult {
        .Stream = deduplicated,
        .StageInputs = condenseResult->StageInputs,
        .StageArgs = condenseResult->StageArgs
    };
}

TMaybe<TCondenseInputResult> CondenseInputToDictByPk(const TExprBase& input, const TKikimrTableDescription& table,
    const TCoLambda& payloadSelector, TExprContext& ctx)
{
    auto condenseResult = CondenseInput(input, ctx);
    if (!condenseResult) {
        return {};
    }

    auto dictStream = Build<TCoMap>(ctx, input.Pos())
        .Input(condenseResult->Stream)
        .Lambda()
            .Args({"row_list"})
            .Body<TCoToDict>()
                .List("row_list")
                .KeySelector(MakeTableKeySelector(table, input.Pos(), ctx))
                .PayloadSelector(payloadSelector)
                .Settings()
                    .Add().Build("One")
                    .Add().Build("Hashed")
                    .Build()
                .Build()
            .Build()
        .Done();

    return TCondenseInputResult {
        .Stream = dictStream,
        .StageInputs = condenseResult->StageInputs,
        .StageArgs = condenseResult->StageArgs
    };
}

TCoLambda MakeTableKeySelector(const TKikimrTableDescription& table, TPositionHandle pos, TExprContext& ctx) {
    auto keySelectorArg = TCoArgument(ctx.NewArgument(pos, "key_selector"));

    TVector<TExprBase> keyTuples;
    for (const auto& key : table.Metadata->KeyColumnNames) {
        auto tuple = Build<TCoNameValueTuple>(ctx, pos)
            .Name().Build(key)
            .Value<TCoMember>()
                .Struct(keySelectorArg)
                .Name().Build(key)
                .Build()
            .Done();

        keyTuples.emplace_back(tuple);
    }

    return Build<TCoLambda>(ctx, pos)
        .Args({keySelectorArg})
        .Body<TCoAsStruct>()
            .Add(keyTuples)
            .Build()
        .Done();
}

TCoLambda MakeRowsPayloadSelector(const TCoAtomList& columns, const TKikimrTableDescription& table,
    TPositionHandle pos, TExprContext& ctx)
{
    for (const auto& key : table.Metadata->KeyColumnNames) {
        auto it = std::find_if(columns.begin(), columns.end(), [&key](const auto& x) { return x.Value() == key; });
        YQL_ENSURE(it != columns.end(), "Key column not found in columns list: " << key);
    }

    auto payloadSelectorArg = TCoArgument(ctx.NewArgument(pos, "payload_selector_row"));
    TVector<TExprBase> payloadTuples;
    payloadTuples.reserve(columns.Size() - table.Metadata->KeyColumnNames.size());
    for (const auto& column : columns) {
        if (table.GetKeyColumnIndex(TString(column))) {
            continue;
        }

        payloadTuples.emplace_back(
            Build<TCoNameValueTuple>(ctx, pos)
                .Name(column)
                .Value<TCoMember>()
                    .Struct(payloadSelectorArg)
                    .Name(column)
                    .Build()
                .Done());
    }

    return Build<TCoLambda>(ctx, pos)
        .Args({payloadSelectorArg})
        .Body<TCoAsStruct>()
            .Add(payloadTuples)
            .Build()
        .Done();
}

} // namespace NKikimr::NKqp::NOpt
