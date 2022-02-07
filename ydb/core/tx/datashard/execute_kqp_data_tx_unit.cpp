#include "datashard_impl.h"
#include "datashard_kqp.h"
#include "datashard_pipeline.h"
#include "execution_unit_ctors.h"
#include "setup_sys_locks.h"

#include <ydb/core/engine/minikql/minikql_engine_host.h>
#include <ydb/core/kqp/rm/kqp_rm.h>

namespace NKikimr {
namespace NDataShard {

using namespace NMiniKQL;

#define LOG_T(stream) LOG_TRACE_S(ctx, NKikimrServices::TX_DATASHARD, stream)
#define LOG_D(stream) LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD, stream)
#define LOG_E(stream) LOG_ERROR_S(ctx, NKikimrServices::TX_DATASHARD, stream)
#define LOG_C(stream) LOG_CRIT_S(ctx, NKikimrServices::TX_DATASHARD, stream)
#define LOG_W(stream) LOG_WARN_S(ctx, NKikimrServices::TX_DATASHARD, stream)

class TExecuteKqpDataTxUnit : public TExecutionUnit {
public:
    TExecuteKqpDataTxUnit(TDataShard& dataShard, TPipeline& pipeline);
    ~TExecuteKqpDataTxUnit() override;

    bool IsReadyToExecute(TOperation::TPtr op) const override;
    EExecutionStatus Execute(TOperation::TPtr op, TTransactionContext& txc, const TActorContext& ctx) override;
    void Complete(TOperation::TPtr op, const TActorContext& ctx) override;

private:
    void AddLocksToResult(TOperation::TPtr op, const TActorContext& ctx);
};

TExecuteKqpDataTxUnit::TExecuteKqpDataTxUnit(TDataShard& dataShard, TPipeline& pipeline)
    : TExecutionUnit(EExecutionUnitKind::ExecuteKqpDataTx, true, dataShard, pipeline) {}

TExecuteKqpDataTxUnit::~TExecuteKqpDataTxUnit() {}

bool TExecuteKqpDataTxUnit::IsReadyToExecute(TOperation::TPtr op) const {
    if (op->Result() || op->HasResultSentFlag() || op->IsImmediate() && WillRejectDataTx(op)) {
        return true;
    }

    if (DataShard.IsStopping()) {
        // Avoid doing any new work when datashard is stopping
        return false;
    }

    return !op->HasRuntimeConflicts();
}

EExecutionStatus TExecuteKqpDataTxUnit::Execute(TOperation::TPtr op, TTransactionContext& txc,
    const TActorContext& ctx)
{
    if (op->Result() || op->HasResultSentFlag() || op->IsImmediate() && CheckRejectDataTx(op, ctx)) {
        return EExecutionStatus::Executed;
    }

    // We remember current time now, but will only count it when transaction succeeds
    TDuration waitExecuteLatency = op->GetCurrentElapsed();
    TDuration waitTotalLatency = op->GetTotalElapsed();

    if (op->IsImmediate()) {
        // Every time we execute immediate transaction we may choose a new mvcc version
        op->MvccReadWriteVersion.reset();
    }

    TSetupSysLocks guardLocks(op, DataShard);
    TActiveTransaction* tx = dynamic_cast<TActiveTransaction*>(op.Get());
    Y_VERIFY_S(tx, "cannot cast operation of kind " << op->GetKind());

    DataShard.ReleaseCache(*tx);

    if (tx->IsTxDataReleased()) {
        switch (Pipeline.RestoreDataTx(tx, txc, ctx)) {
            case ERestoreDataStatus::Ok:
                break;
            case ERestoreDataStatus::Restart:
                return EExecutionStatus::Restart;
            case ERestoreDataStatus::Error:
                Y_FAIL("Failed to restore tx data: %s", tx->GetDataTx()->GetErrors().c_str());
        }
    }

    ui64 tabletId = DataShard.TabletID();
    const TValidatedDataTx::TPtr& dataTx = tx->GetDataTx();

    if (dataTx->CheckCancelled()) {
        tx->ReleaseTxData(txc, ctx);
        BuildResult(op, NKikimrTxDataShard::TEvProposeTransactionResult::CANCELLED)
            ->AddError(NKikimrTxDataShard::TError::EXECUTION_CANCELLED, "Tx was cancelled");

        DataShard.IncCounter(op->IsImmediate() ? COUNTER_IMMEDIATE_TX_CANCELLED : COUNTER_PLANNED_TX_CANCELLED);

        return EExecutionStatus::Executed;
    }

    try {
        auto& kqpTx = dataTx->GetKqpTransaction();

        if (!KqpValidateLocks(tabletId, tx, DataShard.SysLocksTable())) {
            KqpEraseLocks(tabletId, tx, DataShard.SysLocksTable());
            DataShard.SysLocksTable().ApplyLocks();
            return EExecutionStatus::Executed;
        }

        auto& tasksRunner = dataTx->GetKqpTasksRunner();

        auto allocGuard = tasksRunner.BindAllocator(txc.GetMemoryLimit() - dataTx->GetTxSize());

        NKqp::NRm::TKqpResourcesRequest req;
        req.MemoryPool = NKqp::NRm::EKqpMemoryPool::DataQuery;
        req.Memory = txc.GetMemoryLimit();
        ui64 taskId = kqpTx.GetTasks().empty() ? std::numeric_limits<ui64>::max() : kqpTx.GetTasks()[0].GetId();
        NKqp::GetKqpResourceManager()->NotifyExternalResourcesAllocated(tx->GetTxId(), taskId, req);

        Y_DEFER {
            NKqp::GetKqpResourceManager()->NotifyExternalResourcesFreed(tx->GetTxId(), taskId);
        };

        LOG_T("Operation " << *op << " (execute_kqp_data_tx) at " << tabletId
            << " set memory limit " << (txc.GetMemoryLimit() - dataTx->GetTxSize()));

        auto execCtx = DefaultKqpExecutionContext();
        tasksRunner.Prepare(DefaultKqpDataReqMemoryLimits(), *execCtx);

        auto [readVersion, writeVersion] = DataShard.GetReadWriteVersions(tx);
        dataTx->SetReadVersion(readVersion);
        dataTx->SetWriteVersion(writeVersion);

        auto& computeCtx = tx->GetDataTx()->GetKqpComputeCtx();

        op->Result() = KqpCompleteTransaction(ctx, tabletId, op->GetTxId(),
            op->HasKqpAttachedRSFlag() ? nullptr : &op->InReadSets(), dataTx->GetKqpTasks(), tasksRunner, computeCtx);
        op->SetKqpAttachedRSFlag();

        KqpEraseLocks(tabletId, tx, DataShard.SysLocksTable());

        if (dataTx->GetCounters().InvisibleRowSkips) {
            DataShard.SysLocksTable().BreakSetLocks(op->LockTxId());
        }

        AddLocksToResult(op, ctx);

        if (op->IsImmediate() && !op->IsReadOnly()) {
            DataShard.PromoteCompleteEdge(writeVersion.Step, txc);
        }

        if (auto changes = dataTx->GetCollectedChanges()) {
            op->ChangeRecords().reserve(changes.size());
            for (const auto& change : changes) {
                op->ChangeRecords().emplace_back(change.Order(), change.PathId(), change.BodySize());
            }
        }

        KqpUpdateDataShardStatCounters(DataShard, dataTx->GetCounters());
        auto statsMode = kqpTx.GetRuntimeSettings().GetStatsMode();
        if (statsMode >= NYql::NDqProto::DQ_STATS_MODE_BASIC) {
            KqpFillTxStats(DataShard, dataTx->GetCounters(), *op->Result());
            KqpFillStats(DataShard, tasksRunner, computeCtx, statsMode, *op->Result());
        }
    } catch (const TMemoryLimitExceededException&) {
        txc.NotEnoughMemory();

        LOG_T("Operation " << *op << " at " << tabletId
            << " exceeded memory limit " << txc.GetMemoryLimit()
            << " and requests " << txc.GetMemoryLimit() * MEMORY_REQUEST_FACTOR
            << " more for the next try (" << txc.GetNotEnoughMemoryCount() << ")");

        DataShard.IncCounter(DataShard.NotEnoughMemoryCounter(txc.GetNotEnoughMemoryCount()));

        txc.RequestMemory(txc.GetMemoryLimit() * MEMORY_REQUEST_FACTOR);
        tx->ReleaseTxData(txc, ctx);

        return EExecutionStatus::Restart;
    } catch (const TNotReadyTabletException& e) {
        LOG_T("Tablet " << tabletId << " is not ready for " << *op << " execution");

        DataShard.IncCounter(COUNTER_TX_TABLET_NOT_READY);

        ui64 pageFaultCount = tx->IncrementPageFaultCount();
        dataTx->GetKqpComputeCtx().PinPages(dataTx->TxInfo().Keys, pageFaultCount);

        tx->ReleaseTxData(txc, ctx);
        return EExecutionStatus::Restart;
    } catch (const yexception& e) {
        LOG_C("Exception while executing KQP transaction " << *op << " at " << tabletId << ": " << e.what());
        if (op->IsReadOnly() || op->IsImmediate()) {
            tx->ReleaseTxData(txc, ctx);
            BuildResult(op, NKikimrTxDataShard::TEvProposeTransactionResult::EXEC_ERROR)
                ->AddError(NKikimrTxDataShard::TError::UNKNOWN, TStringBuilder() << "Tx was terminated: " << e.what());
            return EExecutionStatus::Executed;
        } else {
            Y_FAIL_S("Unexpected exception in KQP transaction execution: " << e.what());
        }
    }

    Pipeline.AddCommittingOp(op);

    DataShard.IncCounter(COUNTER_WAIT_EXECUTE_LATENCY_MS, waitExecuteLatency.MilliSeconds());
    DataShard.IncCounter(COUNTER_WAIT_TOTAL_LATENCY_MS, waitTotalLatency.MilliSeconds());
    op->ResetCurrentTimer();

    return op->IsReadOnly() ? EExecutionStatus::Executed : EExecutionStatus::ExecutedNoMoreRestarts;
}

void TExecuteKqpDataTxUnit::AddLocksToResult(TOperation::TPtr op, const TActorContext& ctx) {
    auto locks = DataShard.SysLocksTable().ApplyLocks();
    LOG_T("add locks to result: " << locks.size());
    for (const auto& lock : locks) {
        if (lock.IsError()) {
            LOG_NOTICE_S(TActivationContext::AsActorContext(), NKikimrServices::TX_DATASHARD, "Lock is not set for "
                << *op << " at " << DataShard.TabletID() << " lock " << lock);
        }

        op->Result()->AddTxLock(lock.LockId, lock.DataShard, lock.Generation, lock.Counter, lock.SchemeShard,
            lock.PathId);

        LOG_T("add lock to result: " << op->Result()->Record.GetTxLocks().rbegin()->ShortDebugString());
    }
}

void TExecuteKqpDataTxUnit::Complete(TOperation::TPtr, const TActorContext&) {}

THolder<TExecutionUnit> CreateExecuteKqpDataTxUnit(TDataShard& dataShard, TPipeline& pipeline) {
    return THolder(new TExecuteKqpDataTxUnit(dataShard, pipeline));
}

} // namespace NDataShard
} // namespace NKikimr
