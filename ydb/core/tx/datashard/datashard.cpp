#include "datashard_impl.h"
#include "datashard_txs.h"

#include <ydb/core/base/interconnect_channels.h>
#include <ydb/core/engine/minikql/flat_local_tx_factory.h>
#include <ydb/core/scheme/scheme_tablecell.h>
#include <ydb/core/tablet/tablet_counters_protobuf.h>

#include <library/cpp/monlib/service/pages/templates.h>

#include <contrib/libs/apache/arrow/cpp/src/arrow/api.h>

namespace NKikimr {

IActor* CreateDataShard(const TActorId &tablet, TTabletStorageInfo *info) {
    return new NDataShard::TDataShard(tablet, info);
}

namespace NDataShard {

using namespace NSchemeShard;
using namespace NTabletFlatExecutor;

// NOTE: We really want to batch log records by default in datashards!
// But in unittests we want to test both scenarios
bool gAllowLogBatchingDefaultValue = true;

TDuration gDbStatsReportInterval = TDuration::Seconds(10);
ui64 gDbStatsDataSizeResolution = 10*1024*1024;
ui64 gDbStatsRowCountResolution = 100000;

// The first byte is 0x01 so it would fail to parse as an internal tablet protobuf
TStringBuf SnapshotTransferReadSetMagic("\x01SRS", 4);


/**
 * A special subclass of TMiniKQLFactory that uses correct row versions for writes
 */
class TDataShardMiniKQLFactory : public NMiniKQL::TMiniKQLFactory {
public:
    TDataShardMiniKQLFactory(TDataShard* self)
        : Self(self)
    { }

    TRowVersion GetWriteVersion(const TTableId& tableId) const override {
        using Schema = TDataShard::Schema;

        Y_VERIFY_S(tableId.PathId.OwnerId == Self->TabletID(),
            "Unexpected table " << tableId.PathId.OwnerId << ":" << tableId.PathId.LocalPathId
            << " for datashard " << Self->TabletID()
            << " in a local minikql tx");

        if (tableId.PathId.LocalPathId < Schema::MinLocalTid) {
            // System tables are not versioned
            return TRowVersion::Min();
        }

        // Write user tables with a minimal safe version (avoiding snapshots)
        return Self->GetLocalReadWriteVersions().WriteVersion;
    }

    TRowVersion GetReadVersion(const TTableId& tableId) const override {
        using Schema = TDataShard::Schema;

        Y_VERIFY_S(tableId.PathId.OwnerId == Self->TabletID(),
                   "Unexpected table " << tableId.PathId.OwnerId << ":" << tableId.PathId.LocalPathId
                                       << " for datashard " << Self->TabletID()
                                       << " in a local minikql tx");

        if (tableId.PathId.LocalPathId < Schema::MinLocalTid) {
            // System tables are not versioned
            return TRowVersion::Max();
        }

        return Self->GetLocalReadWriteVersions().ReadVersion;
    }

private:
    TDataShard* const Self;
};


class TDatashardKeySampler : public NMiniKQL::IKeyAccessSampler {
    TDataShard& Self;
public:
    TDatashardKeySampler(TDataShard& self) : Self(self)
    {}
    void AddSample(const TTableId& tableId, const TArrayRef<const TCell>& key) override {
        Self.SampleKeyAccess(tableId, key);
    }
};


TDataShard::TDataShard(const TActorId &tablet, TTabletStorageInfo *info)
    : TActor(&TThis::StateInit)
    , TTabletExecutedFlat(info, tablet, new TDataShardMiniKQLFactory(this))
    , PipeClientCacheConfig(new NTabletPipe::TBoundedClientCacheConfig())
    , PipeClientCache(NTabletPipe::CreateBoundedClientCache(PipeClientCacheConfig, GetPipeClientConfig()))
    , ResendReadSetPipeTracker(*PipeClientCache)
    , SchemeShardPipeRetryPolicy({})
    , PathOwnerId(INVALID_TABLET_ID)
    , CurrentSchemeShardId(INVALID_TABLET_ID)
    , LastKnownMediator(INVALID_TABLET_ID)
    , RegistrationSended(false)
    , LoanReturnTracker(info->TabletID)
    , MvccSwitchState(TSwitchState::READY)
    , SplitSnapshotStarted(false)
    , SplitSrcSnapshotSender(this)
    , DstSplitOpId(0)
    , SrcSplitOpId(0)
    , State(TShardState::Uninitialized)
    , LastLocalTid(Schema::MinLocalTid)
    , LastSeqno(1)
    , NextChangeRecordOrder(1)
    , LastChangeRecordGroup(1)
    , TxReadSizeLimit(0)
    , StatisticsDisabled(0)
    , DisabledKeySampler(new NMiniKQL::TNoopKeySampler())
    , EnabledKeySampler(new TDatashardKeySampler(*this))
    , CurrentKeySampler(DisabledKeySampler)
    , TransQueue(this)
    , OutReadSets(this)
    , Pipeline(this)
    , SysLocks(this)
    , SnapshotManager(this)
    , SchemaSnapshotManager(this)
    , DisableByKeyFilter(0, 0, 1)
    , MaxTxInFly(15000, 0, 100000)
    , MaxTxLagMilliseconds(5*60*1000, 0, 30*24*3600*1000ll)
    , CanCancelROWithReadSets(0, 0, 1)
    , PerShardReadSizeLimit(5368709120, 0, 107374182400)
    , CpuUsageReportThreshlodPercent(60, -1, 146)
    , CpuUsageReportIntervalSeconds(60, 0, 365*86400)
    , HighDataSizeReportThreshlodBytes(10ull<<30, -1, Max<i64>())
    , HighDataSizeReportIntervalSeconds(60, 0, 365*86400)
    , DataTxProfileLogThresholdMs(0, 0, 86400000)
    , DataTxProfileBufferThresholdMs(0, 0, 86400000)
    , DataTxProfileBufferSize(0, 1000, 100)
    , ReadColumnsScanEnabled(1, 0, 1)
    , ReadColumnsScanInUserPool(0, 0, 1)
    , BackupReadAheadLo(0, 0, 64*1024*1024)
    , BackupReadAheadHi(0, 0, 128*1024*1024)
    , DataShardSysTables(InitDataShardSysTables(this))
    , ChangeSenderActivator(info->TabletID)
    , ChangeExchangeSplitter(this)
{
    TabletCountersPtr.Reset(new TProtobufTabletCounters<
        ESimpleCounters_descriptor,
        ECumulativeCounters_descriptor,
        EPercentileCounters_descriptor,
        ETxTypes_descriptor
    >());
    TabletCounters = TabletCountersPtr.Get();
}

NTabletPipe::TClientConfig TDataShard::GetPipeClientConfig() {
    NTabletPipe::TClientConfig config;
    config.CheckAliveness = true;
    config.RetryPolicy = {
        .RetryLimitCount = 30,
        .MinRetryTime = TDuration::MilliSeconds(10),
        .MaxRetryTime = TDuration::MilliSeconds(500),
        .BackoffMultiplier = 2,
    };
    return config;
}

void TDataShard::OnDetach(const TActorContext &ctx) {
    Cleanup(ctx);
    LOG_INFO_S(ctx, NKikimrServices::TX_DATASHARD, "OnDetach: " << TabletID());
    return Die(ctx);
}

void TDataShard::OnTabletStop(TEvTablet::TEvTabletStop::TPtr &ev, const TActorContext &ctx) {
    const auto* msg = ev->Get();

    LOG_INFO_S(ctx, NKikimrServices::TX_DATASHARD, "OnTabletStop: " << TabletID() << " reason = " << msg->GetReason());

    if (!IsFollower() && GetState() == TShardState::Ready) {
        if (!Stopping) {
            Stopping = true;
            OnStopGuardStarting(ctx);
            Execute(new TTxStopGuard(this), ctx);
        }

        switch (msg->GetReason()) {
            case TEvTablet::TEvTabletStop::ReasonStop:
            case TEvTablet::TEvTabletStop::ReasonDemoted:
            case TEvTablet::TEvTabletStop::ReasonIsolated:
                // Keep trying to stop gracefully
                return;

            case TEvTablet::TEvTabletStop::ReasonUnknown:
            case TEvTablet::TEvTabletStop::ReasonStorageBlocked:
            case TEvTablet::TEvTabletStop::ReasonStorageFailure:
                // New commits are impossible, stop immediately
                break;
        }
    } else {
        Stopping = true;
    }

    return TTabletExecutedFlat::OnTabletStop(ev, ctx);
}

void TDataShard::TTxStopGuard::Complete(const TActorContext &ctx) {
    Self->OnStopGuardComplete(ctx);
}

void TDataShard::OnStopGuardStarting(const TActorContext &ctx) {
    // Handle immediate ops that have completed BuildAndWaitDependencies
    for (const auto &kv : Pipeline.GetImmediateOps()) {
        const auto &op = kv.second;
        // Send reject result immediately, because we cannot control when
        // a new datashard tablet may start and block us from commiting
        // anything new. The usual progress queue is too slow for that.
        if (!op->Result() && !op->HasResultSentFlag()) {
            auto kind = static_cast<NKikimrTxDataShard::ETransactionKind>(op->GetKind());
            auto rejectStatus = NKikimrTxDataShard::TEvProposeTransactionResult::OVERLOADED;
            TString rejectReason = TStringBuilder()
                    << "Rejecting immediate tx "
                    << op->GetTxId()
                    << " because datashard "
                    << TabletID()
                    << " is restarting";
            auto result = MakeHolder<TEvDataShard::TEvProposeTransactionResult>(
                    kind, TabletID(), op->GetTxId(), rejectStatus);
            result->AddError(NKikimrTxDataShard::TError::WRONG_SHARD_STATE, rejectReason);
            LOG_NOTICE_S(ctx, NKikimrServices::TX_DATASHARD, rejectReason);

            ctx.Send(op->GetTarget(), result.Release(), 0, op->GetCookie());

            IncCounter(COUNTER_PREPARE_OVERLOADED);
            IncCounter(COUNTER_PREPARE_COMPLETE);
            op->SetResultSentFlag();
        }
        // Add op to candidates because IsReadyToExecute just became true
        Pipeline.AddCandidateOp(op);
        PlanQueue.Progress(ctx);
    }

    // Handle prepared ops by notifying about imminent shutdown
    for (const auto &kv : TransQueue.GetTxsInFly()) {
        const auto &op = kv.second;
        if (op->GetTarget() && !op->HasCompletedFlag()) {
            auto notify = MakeHolder<TEvDataShard::TEvProposeTransactionRestart>(
                TabletID(), op->GetTxId());
            ctx.Send(op->GetTarget(), notify.Release(), 0, op->GetCookie());
        }
    }
}

void TDataShard::OnStopGuardComplete(const TActorContext &ctx) {
    // We have cleanly completed the last commit
    ctx.Send(Tablet(), new TEvTablet::TEvTabletStopped());
}

void TDataShard::OnTabletDead(TEvTablet::TEvTabletDead::TPtr &ev, const TActorContext &ctx) {
    Y_UNUSED(ev);
    LOG_INFO_S(ctx, NKikimrServices::TX_DATASHARD, "OnTabletDead: " << TabletID());
    Cleanup(ctx);
    return Die(ctx);
}

void TDataShard::Cleanup(const TActorContext& ctx) {
    //PipeClientCache->Detach(ctx);
    if (RegistrationSended) {
        ctx.Send(MakeMediatorTimecastProxyID(), new TEvMediatorTimecast::TEvUnregisterTablet(TabletID()));
    }

    if (Pipeline.HasRestore()) {
        auto op = Pipeline.FindOp(Pipeline.CurrentSchemaTxId());
        if (op && op->IsWaitingForAsyncJob()) {
            TActiveTransaction* tx = dynamic_cast<TActiveTransaction*>(op.Get());
            Y_VERIFY(tx);
            tx->KillAsyncJobActor(ctx);
        }
    }
}

void TDataShard::OnActivateExecutor(const TActorContext& ctx) {
    LOG_INFO_S(ctx, NKikimrServices::TX_DATASHARD, "TDataShard::OnActivateExecutor: tablet " << TabletID() << " actor " << ctx.SelfID);

    AppData(ctx)->Icb->RegisterSharedControl(DisableByKeyFilter, "DataShardControls.DisableByKeyFilter");
    AppData(ctx)->Icb->RegisterSharedControl(MaxTxInFly, "DataShardControls.MaxTxInFly");
    AppData(ctx)->Icb->RegisterSharedControl(MaxTxLagMilliseconds, "DataShardControls.MaxTxLagMilliseconds");
    AppData(ctx)->Icb->RegisterSharedControl(DataTxProfileLogThresholdMs, "DataShardControls.DataTxProfile.LogThresholdMs");
    AppData(ctx)->Icb->RegisterSharedControl(DataTxProfileBufferThresholdMs, "DataShardControls.DataTxProfile.BufferThresholdMs");
    AppData(ctx)->Icb->RegisterSharedControl(DataTxProfileBufferSize, "DataShardControls.DataTxProfile.BufferSize");

    AppData(ctx)->Icb->RegisterSharedControl(CanCancelROWithReadSets, "DataShardControls.CanCancelROWithReadSets");
    AppData(ctx)->Icb->RegisterSharedControl(PerShardReadSizeLimit, "TxLimitControls.PerShardReadSizeLimit");
    AppData(ctx)->Icb->RegisterSharedControl(CpuUsageReportThreshlodPercent, "DataShardControls.CpuUsageReportThreshlodPercent");
    AppData(ctx)->Icb->RegisterSharedControl(CpuUsageReportIntervalSeconds, "DataShardControls.CpuUsageReportIntervalSeconds");
    AppData(ctx)->Icb->RegisterSharedControl(HighDataSizeReportThreshlodBytes, "DataShardControls.HighDataSizeReportThreshlodBytes");
    AppData(ctx)->Icb->RegisterSharedControl(HighDataSizeReportIntervalSeconds, "DataShardControls.HighDataSizeReportIntervalSeconds");

    AppData(ctx)->Icb->RegisterSharedControl(ReadColumnsScanEnabled, "DataShardControls.ReadColumnsScanEnabled");
    AppData(ctx)->Icb->RegisterSharedControl(ReadColumnsScanInUserPool, "DataShardControls.ReadColumnsScanInUserPool");

    AppData(ctx)->Icb->RegisterSharedControl(BackupReadAheadLo, "DataShardControls.BackupReadAheadLo");
    AppData(ctx)->Icb->RegisterSharedControl(BackupReadAheadHi, "DataShardControls.BackupReadAheadHi");

    // OnActivateExecutor might be called multiple times for a follower
    // but the counters should be initialized only once
    if (TabletCountersPtr) {
        Executor()->RegisterExternalTabletCounters(TabletCountersPtr);
    }
    Y_VERIFY(TabletCounters);

    AllocCounters = TAlignedPagePoolCounters(AppData(ctx)->Counters, "datashard");

    if (!Executor()->GetStats().IsFollower) {
        Execute(CreateTxInitSchema(), ctx);
        Become(&TThis::StateInactive);
    } else {
        SyncConfig();
        State = TShardState::Readonly;
        FollowerState = { };
        Become(&TThis::StateWorkAsFollower);
        SignalTabletActive(ctx);
        LOG_INFO_S(ctx, NKikimrServices::TX_DATASHARD, "Follower switched to work state: " << TabletID());
    }
}

void TDataShard::SwitchToWork(const TActorContext &ctx) {
    SyncConfig();
    PlanQueue.Progress(ctx);
    OutReadSets.ResendAll(ctx);

    Become(&TThis::StateWork);
    LOG_INFO_S(ctx, NKikimrServices::TX_DATASHARD, "Switched to work state "
         << DatashardStateName(State) << " tabletId " << TabletID());

    // Cleanup any removed snapshots from the previous generation
    Execute(new TTxCleanupRemovedSnapshots(this), ctx);

    SignalTabletActive(ctx);
    DoPeriodicTasks(ctx);

    NotifySchemeshard(ctx);
    CheckInitiateBorrowedPartsReturn(ctx);
    CheckStateChange(ctx);
}

void TDataShard::SyncConfig() {
    PipeClientCacheConfig->ClientPoolLimit = PipeClientCachePoolLimit();
    PipeClientCache->PopWhileOverflow();
    // TODO[serxa]: dynamic prepared in fly
    //3=SetDynamicPreparedInFly(Config.GetFlowControl().GetPreparedInFlyMax());
}

void TDataShard::SendRegistrationRequestTimeCast(const TActorContext &ctx) {
    if (RegistrationSended)
        return;

    if (!ProcessingParams)
        return;

    LOG_INFO_S(ctx, NKikimrServices::TX_DATASHARD, "Send registration request to time cast "
         << DatashardStateName(State) << " tabletId " << TabletID()
         << " mediators count is " << ProcessingParams->MediatorsSize()
         << " buckets per mediator " << ProcessingParams->GetTimeCastBucketsPerMediator());

    RegistrationSended = true;
    ctx.Send(MakeMediatorTimecastProxyID(), new TEvMediatorTimecast::TEvRegisterTablet(TabletID(), *ProcessingParams));
}

void TDataShard::PrepareAndSaveOutReadSets(ui64 step,
                                                  ui64 txId,
                                                  const TMap<std::pair<ui64, ui64>, TString>& txOutReadSets,
                                                  TVector<THolder<TEvTxProcessing::TEvReadSet>> &preparedRS,
                                                  TTransactionContext &txc,
                                                  const TActorContext& ctx)
{
    NIceDb::TNiceDb db(txc.DB);
    OutReadSets.Cleanup(db, ctx);
    if (txOutReadSets.empty())
        return;

    ui64 prevSeqno = LastSeqno;
    for (auto& kv : txOutReadSets) {
        ui64 source = kv.first.first;
        ui64 target = kv.first.second;
        TReadSetKey rsKey(txId, TabletID(), source, target);
        if (! OutReadSets.Has(rsKey)) {
            ui64 seqno = LastSeqno++;
            OutReadSets.SaveReadSet(db, seqno, step, rsKey, kv.second);
            preparedRS.push_back(PrepareReadSet(step, txId, source, target, kv.second, seqno));
        }
    }

    if (LastSeqno != prevSeqno) {
        PersistSys(db, Schema::Sys_LastSeqno, LastSeqno);
    }
}

void TDataShard::SendDelayedAcks(const TActorContext& ctx, TVector<THolder<IEventHandle>>& delayedAcks) const {
    for (auto& x : delayedAcks) {
        TEvTxProcessing::TEvReadSetAck* ev = dynamic_cast<TEvTxProcessing::TEvReadSetAck*>(x->GetBase());
        LOG_DEBUG(ctx, NKikimrServices::TX_DATASHARD,
                  "Send delayed Ack RS Ack at %" PRIu64 " %s",
                  TabletID(), ev->ToString().data());
        ctx.ExecutorThread.Send(x.Release());
        IncCounter(COUNTER_ACK_SENT_DELAYED);
    }

    delayedAcks.clear();
}

void TDataShard::SendResult(const TActorContext &ctx,
                                   TOutputOpData::TResultPtr &res,
                                   const TActorId &target,
                                   ui64 step,
                                   ui64 txId)
{
    Y_VERIFY(txId == res->GetTxId(), "%" PRIu64 " vs %" PRIu64, txId, res->GetTxId());

    LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD,
                "Complete [" << step << " : " << txId << "] from " << TabletID()
                << " at tablet " << TabletID() << " send result to client "
                << target <<  ", exec latency: " << res->Record.GetExecLatency()
                << " ms, propose latency: " << res->Record.GetProposeLatency() << " ms");

    ui64 resultSize = res->GetTxResult().size();
    ui32 flags = IEventHandle::MakeFlags(TInterconnectChannels::GetTabletChannel(resultSize), 0);
    ctx.Send(target, res.Release(), flags);
}

void TDataShard::FillExecutionStats(const TExecutionProfile& execProfile, TEvDataShard::TEvProposeTransactionResult& result) const {
    TDuration totalCpuTime;
    for (const auto& unit : execProfile.UnitProfiles) {
        totalCpuTime += unit.second.ExecuteTime;
        totalCpuTime += unit.second.CompleteTime;
    }
    result.Record.MutableTxStats()->MutablePerShardStats()->Clear();
    auto& stats = *result.Record.MutableTxStats()->AddPerShardStats();
    stats.SetShardId(TabletID());
    stats.SetCpuTimeUsec(totalCpuTime.MicroSeconds());
}

ui64 TDataShard::AllocateChangeRecordOrder(NIceDb::TNiceDb& db) {
    const ui64 result = NextChangeRecordOrder++;
    PersistSys(db, Schema::Sys_NextChangeRecordOrder, NextChangeRecordOrder);

    return result;
}

ui64 TDataShard::AllocateChangeRecordGroup(NIceDb::TNiceDb& db) {
    const ui64 now = TInstant::Now().GetValue();
    const ui64 result = now > LastChangeRecordGroup ? now : (LastChangeRecordGroup + 1);

    LastChangeRecordGroup = result;
    PersistSys(db, Schema::Sys_LastChangeRecordGroup, LastChangeRecordGroup);

    return result;
}

void TDataShard::PersistChangeRecord(NIceDb::TNiceDb& db, const TChangeRecord& record) {
    LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::TX_DATASHARD, "PersistChangeRecord"
        << ": record: " << record
        << ", at tablet: " << TabletID());

    db.Table<Schema::ChangeRecords>().Key(record.GetOrder()).Update(
        NIceDb::TUpdate<Schema::ChangeRecords::Group>(record.GetGroup()),
        NIceDb::TUpdate<Schema::ChangeRecords::PlanStep>(record.GetStep()),
        NIceDb::TUpdate<Schema::ChangeRecords::TxId>(record.GetTxId()),
        NIceDb::TUpdate<Schema::ChangeRecords::PathOwnerId>(record.GetPathId().OwnerId),
        NIceDb::TUpdate<Schema::ChangeRecords::LocalPathId>(record.GetPathId().LocalPathId),
        NIceDb::TUpdate<Schema::ChangeRecords::BodySize>(record.GetBody().size()));
    db.Table<Schema::ChangeRecordDetails>().Key(record.GetOrder()).Update(
        NIceDb::TUpdate<Schema::ChangeRecordDetails::Kind>(record.GetKind()),
        NIceDb::TUpdate<Schema::ChangeRecordDetails::Body>(record.GetBody()));
}

void TDataShard::MoveChangeRecord(NIceDb::TNiceDb& db, ui64 order, const TPathId& pathId) {
    LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::TX_DATASHARD, "MoveChangeRecord"
        << ": order: " << order
        << ": pathId: " << pathId
        << ", at tablet: " << TabletID());

    db.Table<Schema::ChangeRecords>().Key(order).Update(
        NIceDb::TUpdate<Schema::ChangeRecords::PathOwnerId>(pathId.OwnerId),
        NIceDb::TUpdate<Schema::ChangeRecords::LocalPathId>(pathId.LocalPathId));
}

void TDataShard::RemoveChangeRecord(NIceDb::TNiceDb& db, ui64 order) {
    LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::TX_DATASHARD, "RemoveChangeRecord"
        << ": order: " << order
        << ", at tablet: " << TabletID());

    auto it = ChangesQueue.find(order);
    if (it != ChangesQueue.end()) {
        Y_VERIFY(it->second <= ChangesQueueBytes);
        ChangesQueueBytes -= it->second;
        ChangesQueue.erase(it);
    }

    db.Table<Schema::ChangeRecords>().Key(order).Delete();
    db.Table<Schema::ChangeRecordDetails>().Key(order).Delete();
}

void TDataShard::EnqueueChangeRecords(TVector<TEvChangeExchange::TEvEnqueueRecords::TRecordInfo>&& records) {
    if (!records) {
        return;
    }

    LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::TX_DATASHARD, "EnqueueChangeRecords"
        << ": at tablet: " << TabletID()
        << ", records: " << JoinSeq(", ", records));

    for (const auto& record : records) {
        if (ChangesQueue.emplace(record.Order, record.BodySize).second) {
            Y_VERIFY(ChangesQueueBytes <= (Max<ui64>() - record.BodySize));
            ChangesQueueBytes += record.BodySize;
        }
    }

    Y_VERIFY(OutChangeSender);
    Send(OutChangeSender, new TEvChangeExchange::TEvEnqueueRecords(std::move(records)));
}

void TDataShard::CreateChangeSender(const TActorContext& ctx) {
    Y_VERIFY(!OutChangeSender);
    OutChangeSender = Register(NDataShard::CreateChangeSender(this));

    LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD, "Change sender created"
        << ": at tablet: " << TabletID()
        << ", actorId: " << OutChangeSender);
}

void TDataShard::MaybeActivateChangeSender(const TActorContext& ctx) {
    LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD, "Trying to activate change sender"
        << ": at tablet: " << TabletID());

    if (ReceiveActivationsFrom) {
        LOG_NOTICE_S(ctx, NKikimrServices::TX_DATASHARD, "Cannot activate change sender"
            << ": at tablet: " << TabletID()
            << ", wait to activation from: " << JoinSeq(", ", ReceiveActivationsFrom));
        return;
    }

    switch (State) {
    case TShardState::WaitScheme:
    case TShardState::SplitDstReceivingSnapshot:
    case TShardState::Offline:
        LOG_INFO_S(ctx, NKikimrServices::TX_DATASHARD, "Cannot activate change sender"
            << ": at tablet: " << TabletID()
            << ", state: " << DatashardStateName(State));
        return;

    case TShardState::SplitSrcMakeSnapshot:
    case TShardState::SplitSrcSendingSnapshot:
    case TShardState::SplitSrcWaitForPartitioningChanged:
    case TShardState::PreOffline:
        if (!ChangesQueue) {
            LOG_INFO_S(ctx, NKikimrServices::TX_DATASHARD, "Cannot activate change sender"
                << ": at tablet: " << TabletID()
                << ", state: " << DatashardStateName(State)
                << ", queue size: " << ChangesQueue.size());
            return;
        }
        break;
    }

    Y_VERIFY(OutChangeSender);
    Send(OutChangeSender, new TEvChangeExchange::TEvActivateSender());

    LOG_INFO_S(ctx, NKikimrServices::TX_DATASHARD, "Change sender activated"
        << ": at tablet: " << TabletID());
}

void TDataShard::KillChangeSender(const TActorContext& ctx) {
    if (OutChangeSender) {
        Send(std::exchange(OutChangeSender, TActorId()), new TEvents::TEvPoison());

        LOG_INFO_S(ctx, NKikimrServices::TX_DATASHARD, "Change sender killed"
            << ": at tablet: " << TabletID());
    }
}

bool TDataShard::LoadChangeRecords(NIceDb::TNiceDb& db, TVector<TEvChangeExchange::TEvEnqueueRecords::TRecordInfo>& changeRecords) {
    using Schema = TDataShard::Schema;

    LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::TX_DATASHARD, "LoadChangeRecords"
        << ": QueueSize: " << ChangesQueue.size()
        << ", at tablet: " << TabletID());

    changeRecords.reserve(ChangesQueue.size());

    auto rowset = db.Table<Schema::ChangeRecords>().Range().Select();
    if (!rowset.IsReady()) {
        return false;
    }

    while (!rowset.EndOfSet()) {
        const ui64 order = rowset.GetValue<Schema::ChangeRecords::Order>();
        const ui64 bodySize = rowset.GetValue<Schema::ChangeRecords::BodySize>();
        const auto pathId = TPathId(
            rowset.GetValue<Schema::ChangeRecords::PathOwnerId>(),
            rowset.GetValue<Schema::ChangeRecords::LocalPathId>()
        );

        changeRecords.emplace_back(order, pathId, bodySize);
        if (!rowset.Next()) {
            return false;
        }
    }

    return true;
}

void TDataShard::PersistSchemeTxResult(NIceDb::TNiceDb &db, const TSchemaOperation &op) {
    db.Table<Schema::SchemaOperations>().Key(op.TxId).Update(
        NIceDb::TUpdate<Schema::SchemaOperations::Success>(op.Success),
        NIceDb::TUpdate<Schema::SchemaOperations::Error>(op.Error),
        NIceDb::TUpdate<Schema::SchemaOperations::DataSize>(op.BytesProcessed),
        NIceDb::TUpdate<Schema::SchemaOperations::Rows>(op.RowsProcessed)
    );
}

void TDataShard::NotifySchemeshard(const TActorContext& ctx, ui64 txId) {
    if (!txId) {
        for (const auto& op : TransQueue.GetSchemaOperations())
            NotifySchemeshard(ctx, op.first);
        return;
    }

    TSchemaOperation * op = TransQueue.FindSchemaTx(txId);
    if (!op || !op->Done)
        return;

    LOG_INFO_S(ctx, NKikimrServices::TX_DATASHARD,
               TabletID() << " Sending notify to schemeshard " << op->TabletId
                << " txId " << txId << " state " << DatashardStateName(State) << " TxInFly " << TxInFly());

    if (op->IsDrop()) {
        Y_VERIFY_S(State == TShardState::PreOffline,
                   TabletID() << " is in wrong state (" << State << ") for drop");
        Y_VERIFY_S(!TxInFly(),
                   TabletID() << " has " << TxInFly() << " txs in-fly "
                   << TransQueue.TxInFlyToString());
    }

    THolder<TEvDataShard::TEvSchemaChanged> event =
        THolder(new TEvDataShard::TEvSchemaChanged(ctx.SelfID, TabletID(), State, op->TxId, op->PlanStep, Generation()));

    switch (op->Type) {
        case TSchemaOperation::ETypeBackup:
        case TSchemaOperation::ETypeRestore: {
            auto* result = event->Record.MutableOpResult();
            result->SetSuccess(op->Success);
            result->SetExplain(op->Error);
            result->SetBytesProcessed(op->BytesProcessed);
            result->SetRowsProcessed(op->RowsProcessed);
            break;
        }
        default:
            break;
    }

    SendViaSchemeshardPipe(ctx, op->TabletId, THolder(event.Release()));
}

bool TDataShard::CheckMediatorAuthorisation(ui64 mediatorId) {
    if (!ProcessingParams || 0 == ProcessingParams->MediatorsSize()) {
        return true;
    }

    auto it = std::find(ProcessingParams->GetMediators().begin(),
                        ProcessingParams->GetMediators().end(),
                        mediatorId);

    return it != ProcessingParams->GetMediators().end();
}

void TDataShard::PersistSys(NIceDb::TNiceDb &db, ui64 key, const TString &value) const {
    db.Table<Schema::Sys>().Key(key).Update(NIceDb::TUpdate<Schema::Sys::Bytes>(value));
}

void TDataShard::PersistSys(NIceDb::TNiceDb& db, ui64 key, ui64 value) const {
    db.Table<Schema::Sys>().Key(key).Update(NIceDb::TUpdate<Schema::Sys::Uint64>(value));
}

void TDataShard::PersistSys(NIceDb::TNiceDb& db, ui64 key, ui32 value) const {
    db.Table<Schema::Sys>().Key(key).Update(NIceDb::TUpdate<Schema::Sys::Uint64>(value));
}

void TDataShard::PersistSys(NIceDb::TNiceDb& db, ui64 key, bool value) const {
    db.Table<Schema::Sys>().Key(key).Update(NIceDb::TUpdate<Schema::Sys::Uint64>(value ? 1 : 0));
}

void TDataShard::PersistUserTable(NIceDb::TNiceDb& db, ui64 tableId, const TUserTable& tableInfo) {
    db.Table<Schema::UserTables>().Key(tableId).Update(
        NIceDb::TUpdate<Schema::UserTables::LocalTid>(tableInfo.LocalTid),
        NIceDb::TUpdate<Schema::UserTables::ShadowTid>(tableInfo.ShadowTid),
        NIceDb::TUpdate<Schema::UserTables::Schema>(tableInfo.GetSchema()));
}

void TDataShard::PersistUserTableFullCompactionTs(NIceDb::TNiceDb& db, ui64 tableId, ui64 ts) {
    db.Table<Schema::UserTablesStats>().Key(tableId).Update<Schema::UserTablesStats::FullCompactionTs>(ts);
}

void TDataShard::PersistMoveUserTable(NIceDb::TNiceDb& db, ui64 prevTableId, ui64 tableId, const TUserTable& tableInfo) {
    db.Table<Schema::UserTables>().Key(prevTableId).Delete();
    PersistUserTable(db, tableId, tableInfo);

    db.Table<Schema::UserTablesStats>().Key(prevTableId).Delete();
    if (tableInfo.Stats.LastFullCompaction) {
        PersistUserTableFullCompactionTs(db, tableId, tableInfo.Stats.LastFullCompaction.Seconds());
    }
}

TUserTable::TPtr TDataShard::AlterTableSchemaVersion(
    const TActorContext&, TTransactionContext& txc,
    const TPathId& pathId, const ui64 tableSchemaVersion, bool persist)
{

    Y_VERIFY(GetPathOwnerId() == pathId.OwnerId);
    ui64 tableId = pathId.LocalPathId;

    Y_VERIFY(TableInfos.contains(tableId));
    auto oldTableInfo = TableInfos[tableId];
    Y_VERIFY(oldTableInfo);

    TUserTable::TPtr newTableInfo = new TUserTable(*oldTableInfo);
    newTableInfo->SetTableSchemaVersion(tableSchemaVersion);

    Y_VERIFY_DEBUG_S(oldTableInfo->GetTableSchemaVersion() < newTableInfo->GetTableSchemaVersion(),
                     "pathId " << pathId
                     << "old version " << oldTableInfo->GetTableSchemaVersion()
                     << "new version " << newTableInfo->GetTableSchemaVersion());

    if (persist) {
        NIceDb::TNiceDb db(txc.DB);
        PersistUserTable(db, tableId, *newTableInfo);
    }

    return newTableInfo;
}

TUserTable::TPtr TDataShard::AlterTableAddIndex(
    const TActorContext& ctx, TTransactionContext& txc,
    const TPathId& pathId, ui64 tableSchemaVersion,
    const NKikimrSchemeOp::TIndexDescription& indexDesc)
{
    auto tableInfo = AlterTableSchemaVersion(ctx, txc, pathId, tableSchemaVersion, false);
    tableInfo->AddIndex(indexDesc);

    NIceDb::TNiceDb db(txc.DB);
    PersistUserTable(db, pathId.LocalPathId, *tableInfo);

    return tableInfo;
}

TUserTable::TPtr TDataShard::AlterTableDropIndex(
    const TActorContext& ctx, TTransactionContext& txc,
    const TPathId& pathId, ui64 tableSchemaVersion,
    const TPathId& indexPathId)
{
    auto tableInfo = AlterTableSchemaVersion(ctx, txc, pathId, tableSchemaVersion, false);
    tableInfo->DropIndex(indexPathId);

    NIceDb::TNiceDb db(txc.DB);
    PersistUserTable(db, pathId.LocalPathId, *tableInfo);

    return tableInfo;
}

TUserTable::TPtr TDataShard::AlterTableAddCdcStream(
    const TActorContext& ctx, TTransactionContext& txc,
    const TPathId& pathId, ui64 tableSchemaVersion,
    const NKikimrSchemeOp::TCdcStreamDescription& streamDesc)
{
    auto tableInfo = AlterTableSchemaVersion(ctx, txc, pathId, tableSchemaVersion, false);
    tableInfo->AddCdcStream(streamDesc);

    NIceDb::TNiceDb db(txc.DB);
    PersistUserTable(db, pathId.LocalPathId, *tableInfo);

    return tableInfo;
}

TUserTable::TPtr TDataShard::AlterTableDisableCdcStream(
    const TActorContext& ctx, TTransactionContext& txc,
    const TPathId& pathId, ui64 tableSchemaVersion,
    const TPathId& streamPathId)
{
    auto tableInfo = AlterTableSchemaVersion(ctx, txc, pathId, tableSchemaVersion, false);
    tableInfo->DisableCdcStream(streamPathId);

    NIceDb::TNiceDb db(txc.DB);
    PersistUserTable(db, pathId.LocalPathId, *tableInfo);

    return tableInfo;
}

TUserTable::TPtr TDataShard::AlterTableDropCdcStream(
    const TActorContext& ctx, TTransactionContext& txc,
    const TPathId& pathId, ui64 tableSchemaVersion,
    const TPathId& streamPathId)
{
    auto tableInfo = AlterTableSchemaVersion(ctx, txc, pathId, tableSchemaVersion, false);
    tableInfo->DropCdcStream(streamPathId);

    NIceDb::TNiceDb db(txc.DB);
    PersistUserTable(db, pathId.LocalPathId, *tableInfo);

    return tableInfo;
}

TUserTable::TPtr TDataShard::CreateUserTable(TTransactionContext& txc,
    const NKikimrSchemeOp::TTableDescription& tableScheme)
{
    const TString mainTableName = TDataShard::Schema::UserTablePrefix + tableScheme.GetName();
    ui64 tableId = tableScheme.GetId_Deprecated();
    if (tableScheme.HasPathId()) {
        Y_VERIFY(GetPathOwnerId() == tableScheme.GetPathId().GetOwnerId() || GetPathOwnerId() == INVALID_TABLET_ID);
        tableId = tableScheme.GetPathId().GetLocalId();
    }
    ui32 localTid = ++LastLocalTid;
    ui32 shadowTid = tableScheme.GetPartitionConfig().GetShadowData() ? ++LastLocalTid : 0;
    TUserTable::TPtr tableInfo = new TUserTable(localTid, tableScheme, shadowTid);

    tableInfo->ApplyCreate(txc, mainTableName, tableScheme.GetPartitionConfig());

    if (shadowTid) {
        const TString shadowTableName = TDataShard::Schema::ShadowTablePrefix + tableScheme.GetName();
        tableInfo->ApplyCreateShadow(txc, shadowTableName, tableScheme.GetPartitionConfig());
    }

    NIceDb::TNiceDb db(txc.DB);

    auto& partConfig = tableScheme.GetPartitionConfig();
    if (partConfig.HasTxReadSizeLimit()) {
        TxReadSizeLimit = partConfig.GetTxReadSizeLimit();
        PersistSys(db, Schema::Sys_TxReadSizeLimit, TxReadSizeLimit);
    }
    if (partConfig.HasDisableStatisticsCalculation()) {
        StatisticsDisabled = partConfig.GetDisableStatisticsCalculation() ? 1 : 0;
        PersistSys(db, Schema::Sys_StatisticsDisabled, StatisticsDisabled);
    }

    Pipeline.UpdateConfig(db, partConfig.GetPipelineConfig());

    if (partConfig.HasKeepSnapshotTimeout())
        SnapshotManager.SetKeepSnapshotTimeout(db, partConfig.GetKeepSnapshotTimeout());

    PersistSys(db, Schema::Sys_LastLocalTid, LastLocalTid);
    PersistUserTable(db, tableId, *tableInfo);

    return tableInfo;
}

THashMap<TPathId, TPathId> TDataShard::GetRemapIndexes(const NKikimrTxDataShard::TMoveTable& move) {
    THashMap<TPathId, TPathId> remap;
    for (const auto& item: move.GetReMapIndexes()) {
        auto prevId = TPathId(item.GetPathId().GetOwnerId(), item.GetPathId().GetLocalId());
        auto newId = TPathId(item.GetDstPathId().GetOwnerId(), item.GetDstPathId().GetLocalId());
        remap[prevId] = newId;
    }
    return remap;
}

TUserTable::TPtr TDataShard::MoveUserTable(const TActorContext& ctx, TTransactionContext& txc,
                                                  const NKikimrTxDataShard::TMoveTable& move)
{
    auto prevId = TPathId(move.GetPathId().GetOwnerId(), move.GetPathId().GetLocalId());
    auto newId = TPathId(move.GetDstPathId().GetOwnerId(), move.GetDstPathId().GetLocalId());

    Y_VERIFY(GetPathOwnerId() == prevId.OwnerId);
    Y_VERIFY(TableInfos.contains(prevId.LocalPathId));

    auto newTableInfo = AlterTableSchemaVersion(ctx, txc, prevId, move.GetTableSchemaVersion(), false);
    newTableInfo->SetPath(move.GetDstPath());

    Y_VERIFY(move.ReMapIndexesSize() == newTableInfo->Indexes.size());
    const THashMap<TPathId, TPathId> remap = GetRemapIndexes(move);

    NKikimrSchemeOp::TTableDescription schema;
    newTableInfo->GetSchema(schema);
    for (auto& indexDesc: *schema.MutableTableIndexes()) {
        Y_VERIFY(indexDesc.HasPathOwnerId() && indexDesc.HasLocalPathId());
        auto prevPathId = TPathId(indexDesc.GetPathOwnerId(), indexDesc.GetLocalPathId());
        Y_VERIFY_S(remap.contains(prevPathId), "no rule how to move index with pathId " << prevPathId); // we should remap all indexes
        auto newPathId = remap.at(prevPathId);

        indexDesc.SetPathOwnerId(newPathId.OwnerId);
        indexDesc.SetLocalPathId(newPathId.LocalPathId);

        newTableInfo->Indexes[newPathId] = newTableInfo->Indexes[prevPathId];
        newTableInfo->Indexes.erase(prevPathId);
    }
    newTableInfo->SetSchema(schema);
    Y_VERIFY(move.ReMapIndexesSize() == newTableInfo->Indexes.size());

    RemoveUserTable(prevId);
    AddUserTable(newId, newTableInfo);

    NIceDb::TNiceDb db(txc.DB);
    PersistMoveUserTable(db, prevId.LocalPathId, newId.LocalPathId, *newTableInfo);
    PersistOwnerPathId(newId.OwnerId, txc);

    return newTableInfo;
}

TUserTable::TPtr TDataShard::AlterUserTable(const TActorContext& ctx, TTransactionContext& txc,
                                                   const NKikimrSchemeOp::TTableDescription& alter)
{
    ui64 tableId = alter.GetId_Deprecated();
    if (alter.HasPathId()) {
        Y_VERIFY(GetPathOwnerId() == alter.GetPathId().GetOwnerId());
        tableId = alter.GetPathId().GetLocalId();
    }
    TUserTable::TCPtr oldTable = TableInfos[tableId];
    Y_VERIFY(oldTable);

    TUserTable::TPtr tableInfo = new TUserTable(*oldTable, alter);
    TString strError;
    tableInfo->ApplyAlter(txc, *oldTable, alter, strError);
    if (strError) {
        LOG_ERROR(ctx, NKikimrServices::TX_DATASHARD,
            "Cannot alter datashard %" PRIu64 " for table %" PRIu64 ": %s",
            TabletID(), tableId, strError.data());
    }

    NIceDb::TNiceDb db(txc.DB);

    if (alter.HasPartitionConfig()) {
        // We are going to update table schema and save it
        NKikimrSchemeOp::TTableDescription tableDescr;
        tableInfo->GetSchema(tableDescr);

        const auto& configDelta = alter.GetPartitionConfig();
        auto& config = *tableDescr.MutablePartitionConfig();

        if (configDelta.HasFreezeState()) {
            auto cmd = configDelta.GetFreezeState();
            State = cmd == NKikimrSchemeOp::EFreezeState::Freeze ? TShardState::Frozen : TShardState::Ready;
            PersistSys(db, Schema::Sys_State, State);
        }

        if (configDelta.HasTxReadSizeLimit()) {
            config.SetTxReadSizeLimit(configDelta.GetTxReadSizeLimit());
            TxReadSizeLimit = configDelta.GetTxReadSizeLimit();
            PersistSys(db, Schema::Sys_TxReadSizeLimit, TxReadSizeLimit);
        }

        if (configDelta.HasDisableStatisticsCalculation()) {
            StatisticsDisabled = configDelta.GetDisableStatisticsCalculation() ? 1 : 0;
            PersistSys(db, Schema::Sys_StatisticsDisabled, StatisticsDisabled);
        }

        if (configDelta.HasPipelineConfig()) {
            config.ClearPipelineConfig();
            config.MutablePipelineConfig()->CopyFrom(configDelta.GetPipelineConfig());
            Pipeline.UpdateConfig(db, configDelta.GetPipelineConfig());
        }

        tableInfo->SetSchema(tableDescr);

        if (configDelta.HasKeepSnapshotTimeout())
            SnapshotManager.SetKeepSnapshotTimeout(db, configDelta.GetKeepSnapshotTimeout());
    }

    PersistUserTable(db, tableId, *tableInfo);

    return tableInfo;
}

void TDataShard::DropUserTable(TTransactionContext& txc, ui64 tableId) {
    auto ti = TableInfos.find(tableId);
    Y_VERIFY(ti != TableInfos.end(), "Table with id %" PRIu64 " doesn't exist on this datashard", tableId);

    NIceDb::TNiceDb db(txc.DB);
    txc.DB.NoMoreReadsForTx();
    txc.DB.Alter().DropTable(ti->second->LocalTid);
    if (ti->second->ShadowTid) {
        txc.DB.Alter().DropTable(ti->second->ShadowTid);
    }
    db.Table<Schema::UserTables>().Key(ti->first).Delete();
    db.Table<Schema::UserTablesStats>().Key(ti->first).Delete();

    TableInfos.erase(ti);
}

void TDataShard::DropAllUserTables(TTransactionContext& txc) {
    NIceDb::TNiceDb db(txc.DB);
    txc.DB.NoMoreReadsForTx();

    // All scheme changes must happen first
    for (const auto& ti : TableInfos) {
        txc.DB.Alter().DropTable(ti.second->LocalTid);
        if (ti.second->ShadowTid) {
            txc.DB.Alter().DropTable(ti.second->ShadowTid);
        }
    }

    // Now remove all snapshots and their info
    SnapshotManager.PersistRemoveAllSnapshots(db);
    for (const auto& ti : TableInfos) {
        db.Table<Schema::UserTables>().Key(ti.first).Delete();
        db.Table<Schema::UserTablesStats>().Key(ti.first).Delete();
    }

    TableInfos.clear();
}

// Deletes user table and all system tables that are transfered during split/merge
// This allows their borrowed parts to be returned to the owner tablet
void TDataShard::PurgeTxTables(TTransactionContext& txc) {
    TVector<ui32> tablesToDrop = {
        Schema::TxMain::TableId,
        Schema::TxDetails::TableId,
        Schema::InReadSets::TableId,
        Schema::PlanQueue::TableId,
        Schema::DeadlineQueue::TableId
    };
    for (ui32 ti : tablesToDrop) {
        txc.DB.Alter().DropTable(ti);
    }

    DropAllUserTables(txc);
}

void TDataShard::SnapshotComplete(TIntrusivePtr<NTabletFlatExecutor::TTableSnapshotContext> snapContext, const TActorContext &ctx) {
    if (auto txSnapContext = dynamic_cast<TTxTableSnapshotContext*>(snapContext.Get())) {
        auto stepOrder = txSnapContext->GetStepOrder();
        auto op = Pipeline.GetActiveOp(stepOrder.TxId);

        Y_VERIFY_DEBUG(op, "The Tx that requested snapshot must be active!");
        if (!op) {
            LOG_CRIT_S(ctx, NKikimrServices::TX_DATASHARD,
                       "Got snapshot for missing operation " << stepOrder
                       << " at " << TabletID());
            return;
        }

        Y_VERIFY(txSnapContext->TablesToSnapshot().size() == 1,
                 "Currently only 1 table can be snapshotted");
        ui32 tableId = txSnapContext->TablesToSnapshot()[0];

        LOG_DEBUG(ctx, NKikimrServices::TX_DATASHARD,
                  "Got snapshot in active state at %" PRIu64 " for table %" PRIu32 " txId %" PRIu64,
                  TabletID(), tableId, stepOrder.TxId);

        op->AddInputSnapshot(snapContext);
        Pipeline.AddCandidateOp(op);
        PlanQueue.Progress(ctx);
        return;
    }

    if (auto splitSnapContext = dynamic_cast<TSplitSnapshotContext*>(snapContext.Get())) {
        Execute(CreateTxSplitSnapshotComplete(splitSnapContext), ctx);
        return;
    }

    Y_FAIL("Unexpected table snapshot context");
}

TUserTable::TSpecialUpdate TDataShard::SpecialUpdates(const NTable::TDatabase& db, const TTableId& tableId) const {
    Y_VERIFY(tableId.PathId.OwnerId == PathOwnerId, "%" PRIu64 " vs %" PRIu64,
             tableId.PathId.OwnerId, PathOwnerId);

    auto it = TableInfos.find(tableId.PathId.LocalPathId);
    Y_VERIFY(it != TableInfos.end());
    const TUserTable& tableInfo = *it->second;
    Y_VERIFY(tableInfo.LocalTid != Max<ui32>());

    TUserTable::TSpecialUpdate ret;

    if (tableInfo.SpecialColTablet != Max<ui32>()) {
        ret.ColIdTablet = tableInfo.SpecialColTablet;
        ret.Tablet = TabletID();

        ret.HasUpdates = true;
    }

    if (tableInfo.SpecialColEpoch != Max<ui32>() || tableInfo.SpecialColUpdateNo != Max<ui32>()) {
        auto dbChange = db.Head(tableInfo.LocalTid);
        ret.ColIdEpoch = tableInfo.SpecialColEpoch;
        ret.ColIdUpdateNo = tableInfo.SpecialColUpdateNo;

        ret.Epoch = dbChange.Epoch.ToCounter();
        ret.UpdateNo = dbChange.Serial;

        ret.HasUpdates = true;
    }

    return ret;
}

void TDataShard::SetTableAccessTime(const TTableId& tableId, TInstant ts) {
    Y_VERIFY(!TSysTables::IsSystemTable(tableId));
    auto iter = TableInfos.find(tableId.PathId.LocalPathId);
    Y_VERIFY(iter != TableInfos.end());
    iter->second->Stats.AccessTime = ts;
}

void TDataShard::SetTableUpdateTime(const TTableId& tableId, TInstant ts) {
    Y_VERIFY(!TSysTables::IsSystemTable(tableId));
    auto iter = TableInfos.find(tableId.PathId.LocalPathId);
    Y_VERIFY(iter != TableInfos.end());
    iter->second->Stats.AccessTime = ts;
    iter->second->Stats.UpdateTime = ts;
}

void TDataShard::SampleKeyAccess(const TTableId& tableId, const TArrayRef<const TCell>& row) {
    Y_VERIFY(!TSysTables::IsSystemTable(tableId));

    auto iter = TableInfos.find(tableId.PathId.LocalPathId);
    Y_VERIFY(iter != TableInfos.end());

    const ui64 samplingKeyPrefixSize = row.size();
    TArrayRef<const TCell> key(row.data(), samplingKeyPrefixSize);
    iter->second->Stats.AccessStats.Add(key);
}

NMiniKQL::IKeyAccessSampler::TPtr TDataShard::GetKeyAccessSampler() {
    return CurrentKeySampler;
}

void TDataShard::EnableKeyAccessSampling(const TActorContext &ctx, TInstant until) {
    if (CurrentKeySampler == DisabledKeySampler) {
        for (auto& table : TableInfos) {
            table.second->Stats.AccessStats.Clear();
        }
        CurrentKeySampler = EnabledKeySampler;
        StartedKeyAccessSamplingAt = AppData(ctx)->TimeProvider->Now();
        LOG_NOTICE_S(ctx, NKikimrServices::TX_DATASHARD, "Started key access sampling at datashard: " << TabletID());
    } else { 
        LOG_NOTICE_S(ctx, NKikimrServices::TX_DATASHARD, "Extended key access sampling at datashard: " << TabletID()); 
    }
    StopKeyAccessSamplingAt = until; 
}

bool TDataShard::OnRenderAppHtmlPage(NMon::TEvRemoteHttpInfo::TPtr ev, const TActorContext &ctx) {
    if (!Executor() || !Executor()->GetStats().IsActive)
        return false;

    if (!ev)
        return true;

    LOG_DEBUG(ctx, NKikimrServices::TX_DATASHARD, "Handle TEvRemoteHttpInfo: %s", ev->Get()->Query.data());

    auto cgi = ev->Get()->Cgi();

    auto action = cgi.Get("action");
    if (action) {
        if (action == "cleanup-borrowed-parts") {
            Execute(CreateTxMonitoringCleanupBorrowedParts(this, ev), ctx);
            return true;
        }

        if (action == "reset-schema-version") {
            Execute(CreateTxMonitoringResetSchemaVersion(this, ev), ctx);
            return true;
        }

        if (action == "key-access-sample") {
            TDuration duration = TDuration::Seconds(120);
            EnableKeyAccessSampling(ctx, ctx.Now() + duration);
            ctx.Send(ev->Sender, new NMon::TEvRemoteHttpInfoRes("Enabled key access sampling for " + duration.ToString()));
            return true;
        }

        ctx.Send(ev->Sender, new NMon::TEvRemoteBinaryInfoRes(NMonitoring::HTTPNOTFOUND));
        return true;
    }

    Execute(CreateTxMonitoring(this, ev), ctx);

    return true;
}

ui64 TDataShard::GetMemoryUsage() const {
    ui64 res = sizeof(TDataShard) + (20 << 10); //basic value
    res += Pipeline.GetInactiveTxSize();
    return res;
}

bool TDataShard::ByKeyFilterDisabled() const {
    return DisableByKeyFilter;
}

bool TDataShard::AllowCancelROwithReadsets() const {
    return CanCancelROWithReadSets;
}

bool TDataShard::IsMvccEnabled() const {
    return SnapshotManager.IsMvccEnabled();
}

TReadWriteVersions TDataShard::GetLocalReadWriteVersions() const {
    if (!IsMvccEnabled())
        return {TRowVersion::Max(), SnapshotManager.GetMinWriteVersion()};

    TRowVersion edge = Max(SnapshotManager.GetCompleteEdge(), SnapshotManager.GetIncompleteEdge());
    if (auto nextOp = Pipeline.GetNextPlannedOp(edge.Step, edge.TxId))
        return TRowVersion(nextOp->GetStep(), nextOp->GetTxId());

    return TRowVersion((++edge).Step, ::Max<ui64>());
}

TRowVersion TDataShard::GetMvccTxVersion(EMvccTxMode mode, TOperation* op) const {
    Y_VERIFY_DEBUG(IsMvccEnabled());

    if (op) {
        if (op->IsMvccSnapshotRead()) {
            return op->GetMvccSnapshot();
        }

        if (op->GetStep()) {
            return TRowVersion(op->GetStep(), op->GetTxId());
        }
    }

    TRowVersion edge;
    TRowVersion readEdge = SnapshotManager.GetCompleteEdge();
    TRowVersion writeEdge = Max(readEdge, SnapshotManager.GetIncompleteEdge());
    switch (mode) {
        case EMvccTxMode::ReadOnly:
            // With read-only transactions we don't need reads to include
            // changes made at the incomplete edge, as that is a point where
            // distributed transactions performed some reads, not writes.
            edge = readEdge;
            break;
        case EMvccTxMode::ReadWrite:
            // With read-write transactions we must choose a point that is
            // greater than both complete and incomplete edges. The reason
            // is that incomplete transactions performed some reads at that
            // point and these snapshot points must be repeatable.
            edge = writeEdge;
            break;
    }

    // If there's any planned operation that is above our edge, it would be a
    // suitable version for a new immediate operation. We effectively try to
    // execute "before" that point if possible.
    if (auto nextOp = Pipeline.GetNextPlannedOp(edge.Step, edge.TxId))
        return TRowVersion(nextOp->GetStep(), nextOp->GetTxId());

    // This is currently active step for immediate writes, not that when
    // writeEdge is equal to some (PlanStep, Max<ui64>()) that means everything
    // up to this point is "fixed" and cannot be changed. In that case we
    // choose at least PlanStep + 1 for new writes.
    ui64 writeStep = Max(MediatorTimeCastEntry ? MediatorTimeCastEntry->Get(TabletID()) : 0, (++writeEdge).Step);
    return TRowVersion(writeStep, ::Max<ui64>());
}

TReadWriteVersions TDataShard::GetReadWriteVersions(TOperation* op) const {
    if (!IsMvccEnabled())
        return {TRowVersion::Max(), SnapshotManager.GetMinWriteVersion()};

    if (op && op->MvccReadWriteVersion) {
        return *op->MvccReadWriteVersion;
    }

    auto mvccVersion = GetMvccTxVersion(EMvccTxMode::ReadWrite, op);

    if (op) {
        op->MvccReadWriteVersion = mvccVersion;
    }

    return mvccVersion;
}

NKikimrTxDataShard::TError::EKind ConvertErrCode(NMiniKQL::IEngineFlat::EResult code) {
    using EResult = NMiniKQL::IEngineFlat::EResult;

    switch (code) {
    case EResult::Ok:
        return NKikimrTxDataShard::TError::OK;
    case EResult::SnapshotNotReady:
        return NKikimrTxDataShard::TError::SNAPSHOT_NOT_READY_YET;
    case EResult::SchemeChanged:
        return NKikimrTxDataShard::TError::SCHEME_CHANGED;
    case EResult::IsReadonly:
        return NKikimrTxDataShard::TError::READONLY;
    case EResult::KeyError:
        return NKikimrTxDataShard::TError::SCHEME_ERROR;
    case EResult::ProgramError:
        return NKikimrTxDataShard::TError::PROGRAM_ERROR;
    case EResult::TooManyData:
        return NKikimrTxDataShard::TError::READ_SIZE_EXECEEDED;
    case EResult::SnapshotNotExist:
        return NKikimrTxDataShard::TError::SNAPSHOT_NOT_EXIST;
    case EResult::ResultTooBig:
        return NKikimrTxDataShard::TError::REPLY_SIZE_EXECEEDED;
    case EResult::Cancelled:
        return NKikimrTxDataShard::TError::EXECUTION_CANCELLED;
    default:
        return NKikimrTxDataShard::TError::UNKNOWN;
    }
}

Ydb::StatusIds::StatusCode ConvertToYdbStatusCode(NKikimrTxDataShard::TError::EKind code) {
    switch (code) {
        case NKikimrTxDataShard::TError::OK:
            return Ydb::StatusIds::SUCCESS;
        case NKikimrTxDataShard::TError::BAD_TX_KIND:
        case NKikimrTxDataShard::TError::SCHEME_ERROR:
        case NKikimrTxDataShard::TError::WRONG_PAYLOAD_TYPE:
        case NKikimrTxDataShard::TError::LEAF_REQUIRED:
        case NKikimrTxDataShard::TError::WRONG_SHARD_STATE:
        case NKikimrTxDataShard::TError::PROGRAM_ERROR:
        case NKikimrTxDataShard::TError::OUT_OF_SPACE:
        case NKikimrTxDataShard::TError::READ_SIZE_EXECEEDED:
        case NKikimrTxDataShard::TError::SHARD_IS_BLOCKED:
        case NKikimrTxDataShard::TError::UNKNOWN:
        case NKikimrTxDataShard::TError::REPLY_SIZE_EXECEEDED:
        case NKikimrTxDataShard::TError::EXECUTION_CANCELLED:
            return Ydb::StatusIds::INTERNAL_ERROR;
        case NKikimrTxDataShard::TError::BAD_ARGUMENT:
        case NKikimrTxDataShard::TError::READONLY:
        case NKikimrTxDataShard::TError::SNAPSHOT_NOT_READY_YET:
        case NKikimrTxDataShard::TError::SCHEME_CHANGED:
        case NKikimrTxDataShard::TError::DUPLICATED_SNAPSHOT_POLICY:
        case NKikimrTxDataShard::TError::MISSING_SNAPSHOT_POLICY:
            return Ydb::StatusIds::BAD_REQUEST;
        case NKikimrTxDataShard::TError::SNAPSHOT_NOT_EXIST:
            return Ydb::StatusIds::NOT_FOUND;
        default:
            return Ydb::StatusIds::GENERIC_ERROR;
    }
}

void TDataShard::Handle(TEvents::TEvGone::TPtr &ev) {
    Actors.erase(ev->Sender);
}

void TDataShard::Handle(TEvents::TEvPoisonPill::TPtr &ev, const TActorContext &ctx) {
    LOG_DEBUG(ctx, NKikimrServices::TX_DATASHARD, "Handle TEvents::TEvPoisonPill");
    Y_UNUSED(ev);
    BecomeBroken(ctx);
}

void TDataShard::Handle(TEvDataShard::TEvGetShardState::TPtr &ev, const TActorContext &ctx) {
    Execute(new TTxGetShardState(this, ev), ctx);
}

void TDataShard::Handle(TEvDataShard::TEvSchemaChangedResult::TPtr& ev, const TActorContext& ctx) {
    LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD,
                "Handle TEvSchemaChangedResult " << ev->Get()->Record.GetTxId()
                << "  datashard " << TabletID()
                << " state " << DatashardStateName(State));
    Execute(CreateTxSchemaChanged(ev), ctx);
}

void TDataShard::Handle(TEvDataShard::TEvStateChangedResult::TPtr& ev, const TActorContext& ctx) {
    Y_UNUSED(ev);
    LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD,
                "Handle TEvStateChangedResult "
                << "  datashard " << TabletID()
                << " state " << DatashardStateName(State));
    // TODO: implement
    NTabletPipe::CloseAndForgetClient(SelfId(), StateReportPipe);
}

bool TDataShard::CheckDataTxReject(const TString& opDescr,
                                          const TActorContext &ctx,
                                          NKikimrTxDataShard::TEvProposeTransactionResult::EStatus &rejectStatus,
                                          TString &reason)
{
    bool reject = false;
    rejectStatus = NKikimrTxDataShard::TEvProposeTransactionResult::OVERLOADED;
    TVector<TString> rejectReasons;

    // In v0.5 reject all transactions on split Src after receiving EvSplit
    if (State == TShardState::SplitSrcWaitForNoTxInFlight ||
        State == TShardState::SplitSrcMakeSnapshot ||
        State == TShardState::SplitSrcSendingSnapshot ||
        State == TShardState::SplitSrcWaitForPartitioningChanged) {
        reject = true;
        rejectReasons.push_back(TStringBuilder()
            << "is in process of split opId " << SrcSplitOpId
            << " state " << DatashardStateName(State)
            << " (wrong shard state)");
    } else if (State == TShardState::SplitDstReceivingSnapshot) {
        reject = true;
        rejectReasons.push_back(TStringBuilder()
            << "is in process of split opId " << DstSplitOpId
            << " state " << DatashardStateName(State));
    } else if (State == TShardState::PreOffline || State == TShardState::Offline) {
        reject = true;
        rejectStatus = NKikimrTxDataShard::TEvProposeTransactionResult::ERROR;
        rejectReasons.push_back("is in a pre/offline state assuming this is due to a finished split (wrong shard state)");
    } else if (MvccSwitchState == TSwitchState::SWITCHING) {
        reject = true;
        rejectReasons.push_back(TStringBuilder()
            << "is in process of mvcc state change"
            << " state " << DatashardStateName(State));
    }

    if (Pipeline.HasDrop()) {
        reject = true;
        rejectReasons.push_back("is in process of drop");
        rejectStatus = NKikimrTxDataShard::TEvProposeTransactionResult::ERROR;
    }

    ui64 txInfly = TxInFly();
    TDuration lag = GetDataTxCompleteLag();
    if (txInfly > 1 && lag > TDuration::MilliSeconds(MaxTxLagMilliseconds)) {
        reject = true;
        rejectReasons.push_back(TStringBuilder()
            << "lags behind, lag: " << lag
            << " in-flight tx count: " << txInfly);
    }

    const float rejectProbabilty = Executor()->GetRejectProbability();
    if (!reject && rejectProbabilty > 0) {
        float rnd = AppData(ctx)->RandomProvider->GenRandReal2();
        reject |= (rnd < rejectProbabilty);
        if (reject)
            rejectReasons.push_back("decided to reject due to given RejectProbability");
    }

    size_t totalInFly = (TxInFly() + ImmediateInFly() + ProposeQueue.Size() + TxWaiting());
    if (totalInFly > GetMaxTxInFly()) {
        reject = true;
        rejectReasons.push_back("MaxTxInFly was exceeded");
    }

    if (!reject && Stopping) {
        reject = true;
        rejectReasons.push_back("is restarting");
    }

    if (!reject) {
        for (auto& it : TableInfos) {
            if (it.second->IsBackup) {
                reject = true;
                rejectReasons.push_back("is a backup table");
                rejectStatus = NKikimrTxDataShard::TEvProposeTransactionResult::ERROR;
                break;
            }
        }
    }

    if (reject) {
        reason = TStringBuilder()
            << "Rejecting " << opDescr
            << " because datashard " << TabletID() << ": "
            << JoinSeq("; ", rejectReasons);
    }

    return reject;
}

bool TDataShard::CheckDataTxRejectAndReply(TEvDataShard::TEvProposeTransaction* msg, const TActorContext& ctx)
{
    switch (msg->GetTxKind()) {
        case NKikimrTxDataShard::TX_KIND_DATA:
        case NKikimrTxDataShard::TX_KIND_SCAN:
        case NKikimrTxDataShard::TX_KIND_SNAPSHOT:
        case NKikimrTxDataShard::TX_KIND_DISTRIBUTED_ERASE:
        case NKikimrTxDataShard::TX_KIND_COMMIT_WRITES:
            break;
        default:
            return false;
    }

    TString txDescr = TStringBuilder() << "data TxId " << msg->GetTxId();

    NKikimrTxDataShard::TEvProposeTransactionResult::EStatus rejectStatus;
    TString rejectReason;
    bool reject = CheckDataTxReject(txDescr, ctx, rejectStatus, rejectReason);

    if (reject) {
        THolder<TEvDataShard::TEvProposeTransactionResult> result =
            THolder(new TEvDataShard::TEvProposeTransactionResult(msg->GetTxKind(),
                                                            TabletID(),
                                                            msg->GetTxId(),
                                                            rejectStatus));

        result->AddError(NKikimrTxDataShard::TError::WRONG_SHARD_STATE, rejectReason);
        LOG_NOTICE_S(ctx, NKikimrServices::TX_DATASHARD, rejectReason);

        ctx.Send(msg->GetSource(), result.Release());
        IncCounter(COUNTER_PREPARE_OVERLOADED);
        IncCounter(COUNTER_PREPARE_COMPLETE);
        return true;
    }

    return false;
}

void TDataShard::UpdateProposeQueueSize() const {
    SetCounter(COUNTER_PROPOSE_QUEUE_SIZE, ProposeQueue.Size() + DelayedProposeQueue.size() + Pipeline.WaitingTxs());
}

void TDataShard::Handle(TEvDataShard::TEvProposeTransaction::TPtr &ev, const TActorContext &ctx) {
    if (Pipeline.HasProposeDelayers()) {
        LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD,
            "Handle TEvProposeTransaction delayed at " << TabletID() << " until dependency graph is restored");
        DelayedProposeQueue.emplace_back().Reset(ev.Release());
        UpdateProposeQueueSize();
        return;
    }

    if (CheckTxNeedWait(ev)) {
         LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD,
            "Handle TEvProposeTransaction delayed at " << TabletID() << " until interesting plan step will come");
        if (Pipeline.AddWaitingTxOp(ev, ctx)) {
            UpdateProposeQueueSize();
            return;
        }
    }

    IncCounter(COUNTER_PREPARE_REQUEST);

    if (CheckDataTxRejectAndReply(ev->Get(), ctx)) {
        return;
    }

    switch (ev->Get()->GetTxKind()) {
    case NKikimrTxDataShard::TX_KIND_DATA:
    case NKikimrTxDataShard::TX_KIND_SCAN:
    case NKikimrTxDataShard::TX_KIND_SNAPSHOT:
    case NKikimrTxDataShard::TX_KIND_DISTRIBUTED_ERASE:
    case NKikimrTxDataShard::TX_KIND_COMMIT_WRITES:
        ProposeTransaction(std::move(ev), ctx);
        return;
    case NKikimrTxDataShard::TX_KIND_SCHEME:
        ProposeTransaction(std::move(ev), ctx);
        return;
    default:
        break;
    }

    THolder<TEvDataShard::TEvProposeTransactionResult> result
        = THolder(new TEvDataShard::TEvProposeTransactionResult(ev->Get()->GetTxKind(),
                                                        TabletID(),
                                                        ev->Get()->GetTxId(),
                                                        NKikimrTxDataShard::TEvProposeTransactionResult::ERROR));
    result->AddError(NKikimrTxDataShard::TError::BAD_TX_KIND, "Unknown kind of transaction");
    ctx.Send(ev->Get()->GetSource(), result.Release());
    IncCounter(COUNTER_PREPARE_ERROR);
    IncCounter(COUNTER_PREPARE_COMPLETE);

    // TODO[serxa]: wake up! dont sleep! maybe...
    //Executor()->WakeUp(ctx);
}

void TDataShard::Handle(TEvDataShard::TEvProposeTransactionAttach::TPtr &ev, const TActorContext &ctx) {
    const auto &record = ev->Get()->Record;
    const ui64 txId = record.GetTxId();
    NKikimrProto::EReplyStatus status = NKikimrProto::NODATA;

    auto op = TransQueue.FindTxInFly(txId);
    if (!op) {
        op = Pipeline.FindCompletingOp(txId);
    }

    if (op && op->GetTarget() == ev->Sender && !op->IsImmediate() && op->HasStoredFlag() && !op->HasResultSentFlag()) {
        // This transaction is expected to send reply eventually
        status = NKikimrProto::OK;
    }

    ctx.Send(ev->Sender, new TEvDataShard::TEvProposeTransactionAttachResult(TabletID(), txId, status), 0, ev->Cookie);
}

void TDataShard::HandleAsFollower(TEvDataShard::TEvProposeTransaction::TPtr &ev, const TActorContext &ctx) {
    IncCounter(COUNTER_PREPARE_REQUEST);

    if (TxInFly() > GetMaxTxInFly()) {
        THolder<TEvDataShard::TEvProposeTransactionResult> result =
            THolder(new TEvDataShard::TEvProposeTransactionResult(ev->Get()->GetTxKind(), TabletID(),
                ev->Get()->GetTxId(), NKikimrTxDataShard::TEvProposeTransactionResult::OVERLOADED));
        ctx.Send(ev->Get()->GetSource(), result.Release());
        IncCounter(COUNTER_PREPARE_OVERLOADED);
        IncCounter(COUNTER_PREPARE_COMPLETE);
        return;
    }

    if (ev->Get()->GetTxKind() == NKikimrTxDataShard::TX_KIND_DATA) {
        ProposeTransaction(std::move(ev), ctx);
        return;
    }

    THolder<TEvDataShard::TEvProposeTransactionResult> result
        = THolder(new TEvDataShard::TEvProposeTransactionResult(ev->Get()->GetTxKind(),
                                                        TabletID(),
                                                        ev->Get()->GetTxId(),
                                                        NKikimrTxDataShard::TEvProposeTransactionResult::ERROR));
    result->AddError(NKikimrTxDataShard::TError::BAD_TX_KIND, "Unsupported transaction kind");
    ctx.Send(ev->Get()->GetSource(), result.Release());
    IncCounter(COUNTER_PREPARE_ERROR);
    IncCounter(COUNTER_PREPARE_COMPLETE);
}

void TDataShard::CheckDelayedProposeQueue(const TActorContext &ctx) {
    if (DelayedProposeQueue && !Pipeline.HasProposeDelayers()) {
        for (auto& ev : DelayedProposeQueue) {
            ctx.ExecutorThread.Send(ev.Release());
        }
        DelayedProposeQueue.clear();
        DelayedProposeQueue.shrink_to_fit();
        UpdateProposeQueueSize();
    }
}

void TDataShard::ProposeTransaction(TEvDataShard::TEvProposeTransaction::TPtr &&ev, const TActorContext &ctx) {
    bool mayRunImmediate = false;

    if ((ev->Get()->GetFlags() & TTxFlags::Immediate) &&
        !(ev->Get()->GetFlags() & TTxFlags::ForceOnline) &&
        ev->Get()->GetTxKind() == NKikimrTxDataShard::TX_KIND_DATA)
    {
        // This transaction may run in immediate mode
        mayRunImmediate = true;
    }

    if (mayRunImmediate) {
        // Enqueue immediate transactions so they don't starve existing operations
        ProposeQueue.Enqueue(std::move(ev), TAppData::TimeProvider->Now(), NextTieBreakerIndex++, ctx);
        UpdateProposeQueueSize();
    } else {
        // Prepare planned transactions as soon as possible
        Execute(new TTxProposeTransactionBase(this, std::move(ev), TAppData::TimeProvider->Now(), NextTieBreakerIndex++, /* delayed */ false), ctx);
    }
}

void TDataShard::Handle(TEvTxProcessing::TEvPlanStep::TPtr &ev, const TActorContext &ctx) {
    ui64 srcMediatorId = ev->Get()->Record.GetMediatorID();
    if (!CheckMediatorAuthorisation(srcMediatorId)) {
        LOG_CRIT_S(ctx, NKikimrServices::TX_DATASHARD, "tablet " << TabletID() <<
                   " receive PlanStep " << ev->Get()->Record.GetStep() <<
                   " from unauthorized mediator " << srcMediatorId);
        BecomeBroken(ctx);
        return;
    }

    Execute(new TTxPlanStep(this, ev), ctx);
}

void TDataShard::Handle(TEvTxProcessing::TEvReadSet::TPtr &ev, const TActorContext &ctx) {
    ui64 sender = ev->Get()->Record.GetTabletSource();
    ui64 dest = ev->Get()->Record.GetTabletDest();
    ui64 producer = ev->Get()->Record.GetTabletProducer();
    ui64 txId = ev->Get()->Record.GetTxId();
    LOG_DEBUG(ctx, NKikimrServices::TX_DATASHARD, "Receive RS at %" PRIu64 " source %" PRIu64 " dest %" PRIu64 " producer %" PRIu64 " txId %" PRIu64,
              TabletID(), sender, dest, producer, txId);
    IncCounter(COUNTER_READSET_RECEIVED_COUNT);
    IncCounter(COUNTER_READSET_RECEIVED_SIZE, ev->Get()->Record.GetReadSet().size());
    Execute(new TTxReadSet(this, ev), ctx);
}

void TDataShard::Handle(TEvTxProcessing::TEvReadSetAck::TPtr &ev, const TActorContext &ctx) {
    OutReadSets.SaveAck(ctx, ev->Release());

    // progress one more Tx to force delayed schema operations
    if (Pipeline.HasSchemaOperation() && OutReadSets.Empty()) {
        // TODO: wait for empty OutRS in a separate unit?
        Pipeline.AddCandidateUnit(EExecutionUnitKind::PlanQueue);
        PlanQueue.Progress(ctx);
    }

    CheckStateChange(ctx);
}

void TDataShard::Handle(TEvPrivate::TEvProgressTransaction::TPtr &ev, const TActorContext &ctx) {
    Y_UNUSED(ev);
    IncCounter(COUNTER_TX_PROGRESS_EV);
    ExecuteProgressTx(ctx);
}

void TDataShard::Handle(TEvPrivate::TEvDelayedProposeTransaction::TPtr &ev, const TActorContext &ctx) {
    Y_UNUSED(ev);
    IncCounter(COUNTER_PROPOSE_QUEUE_EV);

    if (ProposeQueue) {
        auto item = ProposeQueue.Dequeue();
        UpdateProposeQueueSize();

        TDuration latency = TAppData::TimeProvider->Now() - item.ReceivedAt;
        IncCounter(COUNTER_PROPOSE_QUEUE_LATENCY, latency);

        if (!item.Cancelled) {
            // N.B. we don't call ProposeQueue.Reset(), tx will Ack() on its first Execute()
            Execute(new TTxProposeTransactionBase(this, std::move(item.Event), item.ReceivedAt, item.TieBreakerIndex, /* delayed */ true), ctx);
            return;
        }

        TActorId target = item.Event->Get()->GetSource();
        ui64 cookie = item.Event->Cookie;
        auto kind = item.Event->Get()->GetTxKind();
        auto txId = item.Event->Get()->GetTxId();
        auto result = new TEvDataShard::TEvProposeTransactionResult(
                kind, TabletID(), txId,
                NKikimrTxDataShard::TEvProposeTransactionResult::CANCELLED);
        ctx.Send(target, result, 0, cookie);
    }

    // N.B. Ack directly since we didn't start any delayed transactions
    ProposeQueue.Ack(ctx);
}

void TDataShard::Handle(TEvPrivate::TEvProgressResendReadSet::TPtr &ev, const TActorContext &ctx) {
    ResendReadSetQueue.Reset(ctx);
    Execute(new TTxProgressResendRS(this, ev->Get()->Seqno), ctx);
}

void TDataShard::Handle(TEvPrivate::TEvRegisterScanActor::TPtr &ev, const TActorContext &ctx) {
    ui64 txId = ev->Get()->TxId;
    auto op = Pipeline.FindOp(txId);

    if (!op) {
        LOG_INFO_S(ctx, NKikimrServices::TX_DATASHARD,
                   "Cannot find op " << txId << " to register scan actor");
        return;
    }

    if (!op->IsReadTable()) {
        LOG_INFO_S(ctx, NKikimrServices::TX_DATASHARD,
                   "Cannot register scan actor for op " << txId
                   << " of kind " << op->GetKind());
        return;
    }

    TActiveTransaction *tx = dynamic_cast<TActiveTransaction*>(op.Get());
    Y_VERIFY_S(tx, "cannot cast operation of kind " << op->GetKind());

    tx->SetScanActor(ev->Sender);
}

void TDataShard::Handle(TEvPrivate::TEvScanStats::TPtr& ev, const TActorContext &ctx) {
    Y_UNUSED(ctx);

    TabletCounters->Cumulative()[COUNTER_SCANNED_ROWS].Increment(ev->Get()->Rows);
    TabletCounters->Cumulative()[COUNTER_SCANNED_BYTES].Increment(ev->Get()->Bytes);
}

void TDataShard::Handle(TEvPrivate::TEvPersistScanState::TPtr& ev, const TActorContext &ctx) {
    TabletCounters->Cumulative()[COUNTER_SCANNED_ROWS].Increment(ev->Get()->Rows);
    TabletCounters->Cumulative()[COUNTER_SCANNED_BYTES].Increment(ev->Get()->Bytes);
    Execute(new TTxStoreScanState(this, ev), ctx);
}

void TDataShard::Handle(TEvTabletPipe::TEvClientConnected::TPtr &ev, const TActorContext &ctx) {
    Y_VERIFY(ev->Get()->Leader, "Unexpectedly connected to follower of tablet %" PRIu64, ev->Get()->TabletId);

    if (ev->Get()->ClientId == SchemeShardPipe) {
        if (!TransQueue.HasNotAckedSchemaTx()) {
            LOG_ERROR(ctx, NKikimrServices::TX_DATASHARD,
                "Datashard's schemeshard pipe connected while no messages to sent at %" PRIu64, TabletID());
        }
        TEvTabletPipe::TEvClientConnected *msg = ev->Get();
        if (msg->Status != NKikimrProto::OK) {
            SchemeShardPipe = TActorId();
            NotifySchemeshard(ctx);
        }
        return;
    }

    if (ev->Get()->Status != NKikimrProto::OK) {
        if (ev->Get()->ClientId == StateReportPipe) {
            StateReportPipe = TActorId();
            ReportState(ctx, State);
            return;
        }

        if (ev->Get()->ClientId == DbStatsReportPipe) {
            DbStatsReportPipe = TActorId();
            return;
        }

        if (ev->Get()->ClientId == TableResolvePipe) {
            TableResolvePipe = TActorId();
            ResolveTablePath(ctx);
            return;
        }
    }

    if (LoanReturnTracker.Has(ev->Get()->TabletId, ev->Get()->ClientId)) {
        if (ev->Get()->Status != NKikimrProto::OK) {
            if (!ev->Get()->Dead) {
                LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD,
                          "Resending loan returns from " << TabletID() << " to " << ev->Get()->TabletId);
                LoanReturnTracker.ResendLoans(ev->Get()->TabletId, ctx);
            } else {
                LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD,
                          "Auto-Acking loan returns to dead " << ev->Get()->TabletId << " from " << TabletID());
                LoanReturnTracker.AutoAckLoans(ev->Get()->TabletId, ctx);
            }
        }
    }

    // Resend split-related messages in needed
    if (SplitSrcSnapshotSender.Has(ev->Get()->TabletId, ev->Get()->ClientId)) {
        if (ev->Get()->Status != NKikimrProto::OK) {
            SplitSrcSnapshotSender.DoSend(ev->Get()->TabletId, ctx);
        }
    }

    if (ChangeSenderActivator.Has(ev->Get()->TabletId, ev->Get()->ClientId)) {
        if (ev->Get()->Status != NKikimrProto::OK) {
            if (!ev->Get()->Dead) {
                ChangeSenderActivator.DoSend(ev->Get()->TabletId, ctx);
            } else {
                ChangeSenderActivator.AutoAck(ev->Get()->TabletId, ctx);
            }
        }
    }

    if (!PipeClientCache->OnConnect(ev)) {
        if (ev->Get()->Dead) {
            AckRSToDeletedTablet(ev->Get()->TabletId, ctx);
        } else {
            LOG_NOTICE(ctx, NKikimrServices::TX_DATASHARD, "Failed to connect to tablet %" PRIu64 " from tablet %" PRIu64, ev->Get()->TabletId, TabletID());
            RestartPipeRS(ev->Get()->TabletId, ctx);
        }
    } else {
        LOG_DEBUG(ctx, NKikimrServices::TX_DATASHARD, "Connected to tablet %" PRIu64 " from tablet %" PRIu64, ev->Get()->TabletId, TabletID());
    }
}

void TDataShard::Handle(TEvTabletPipe::TEvClientDestroyed::TPtr &ev, const TActorContext &ctx) {
    if (ev->Get()->ClientId == SchemeShardPipe) {
        if (!TransQueue.HasNotAckedSchemaTx()) {
            LOG_ERROR(ctx, NKikimrServices::TX_DATASHARD,
                "Datashard's schemeshard pipe destroyed while no messages to sent at %" PRIu64, TabletID());
        }
        SchemeShardPipe = TActorId();
        NotifySchemeshard(ctx);
        return;
    }

    if (ev->Get()->ClientId == StateReportPipe) {
        StateReportPipe = TActorId();
        ReportState(ctx, State);
        return;
    }

    if (ev->Get()->ClientId == DbStatsReportPipe) {
        DbStatsReportPipe = TActorId();
        return;
    }

    // Resend loan-related messages in needed
    if (LoanReturnTracker.Has(ev->Get()->TabletId, ev->Get()->ClientId)) {
        LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD,
                    "Resending loan returns from " << TabletID() << " to " << ev->Get()->TabletId);
        LoanReturnTracker.ResendLoans(ev->Get()->TabletId, ctx);
    }

    // Resend split-related messages in needed
    if (SplitSrcSnapshotSender.Has(ev->Get()->TabletId, ev->Get()->ClientId)) {
        SplitSrcSnapshotSender.DoSend(ev->Get()->TabletId, ctx);
    }

    if (ChangeSenderActivator.Has(ev->Get()->TabletId, ev->Get()->ClientId)) {
        ChangeSenderActivator.DoSend(ev->Get()->TabletId, ctx);
    }

    LOG_DEBUG(ctx, NKikimrServices::TX_DATASHARD, "Client pipe to tablet %" PRIu64 " from %" PRIu64 " is reset", ev->Get()->TabletId, TabletID());
    PipeClientCache->OnDisconnect(ev);
    RestartPipeRS(ev->Get()->TabletId, ctx);
}

void TDataShard::RestartPipeRS(ui64 tabletId, const TActorContext& ctx) {
    for (auto seqno : ResendReadSetPipeTracker.FindTx(tabletId)) {
        LOG_DEBUG(ctx, NKikimrServices::TX_DATASHARD, "Pipe reset to tablet %" PRIu64 " caused resend of readset %" PRIu64
            " at tablet %" PRIu64, tabletId, seqno, TabletID());

        ResendReadSetQueue.Progress(seqno, ctx);
    }
}

void TDataShard::AckRSToDeletedTablet(ui64 tabletId, const TActorContext& ctx) {
    for (auto seqno : ResendReadSetPipeTracker.FindTx(tabletId)) {
        LOG_DEBUG(ctx, NKikimrServices::TX_DATASHARD, "Pipe reset to dead tablet %" PRIu64 " caused ack of readset %" PRIu64
            " at tablet %" PRIu64, tabletId, seqno, TabletID());

        OutReadSets.AckForDeletedDestination(tabletId, seqno, ctx);

        // progress one more Tx to force delayed schema operations
        if (Pipeline.HasSchemaOperation() && OutReadSets.Empty()) {
            // TODO: wait for empty OutRS in a separate unit?
            Pipeline.AddCandidateUnit(EExecutionUnitKind::PlanQueue);
            PlanQueue.Progress(ctx);
        }
    }
    CheckStateChange(ctx);
}

void TDataShard::Handle(TEvTabletPipe::TEvServerConnected::TPtr &ev, const TActorContext &ctx) {
    Y_UNUSED(ev); Y_UNUSED(ctx);
    LOG_DEBUG(ctx, NKikimrServices::TX_DATASHARD, "Server connected at tablet %s %" PRIu64 ,
              Executor()->GetStats().IsFollower ? "follower" : "leader", ev->Get()->TabletId);
}

void TDataShard::Handle(TEvTabletPipe::TEvServerDisconnected::TPtr &ev, const TActorContext &ctx) {
    Y_UNUSED(ev); Y_UNUSED(ctx);
}

void TDataShard::Handle(TEvMediatorTimecast::TEvRegisterTabletResult::TPtr& ev, const TActorContext& ctx) {
    LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD,
                "Got TEvMediatorTimecast::TEvRegisterTabletResult at " << TabletID()
                << " time " << ev->Get()->Entry->Get(TabletID()));
    Y_VERIFY(ev->Get()->TabletId == TabletID());
    MediatorTimeCastEntry = ev->Get()->Entry;
    Y_VERIFY(MediatorTimeCastEntry);

    Pipeline.ActivateWaitingTxOps(ctx);
}

void TDataShard::Handle(TEvMediatorTimecast::TEvNotifyPlanStep::TPtr& ev, const TActorContext& ctx) {
    const auto* msg = ev->Get();
    Y_VERIFY(msg->TabletId == TabletID());

    Y_VERIFY(MediatorTimeCastEntry);
    ui64 step = MediatorTimeCastEntry->Get(TabletID());
    LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD, "Notified by mediator time cast with PlanStep# " << step << " at tablet " << TabletID());

    for (auto it = MediatorTimeCastWaitingSteps.begin(); it != MediatorTimeCastWaitingSteps.end() && *it <= step;)
        it = MediatorTimeCastWaitingSteps.erase(it);

    Pipeline.ActivateWaitingTxOps(ctx);
}

bool TDataShard::WaitPlanStep(ui64 step) {
    if (step <= Pipeline.GetLastPlannedTx().Step)
        return false;

    if (MediatorTimeCastEntry && step <= MediatorTimeCastEntry->Get(TabletID()))
        return false;

    if (!RegistrationSended)
        return false;

    if (MediatorTimeCastWaitingSteps.empty() || step < *MediatorTimeCastWaitingSteps.begin()) {
        MediatorTimeCastWaitingSteps.insert(step);
        Send(MakeMediatorTimecastProxyID(), new TEvMediatorTimecast::TEvWaitPlanStep(TabletID(), step));
        LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::TX_DATASHARD, "Waiting for PlanStep# " << step << " from mediator time cast");
        return true;
    }

    return false;
}

bool TDataShard::CheckTxNeedWait(const TEvDataShard::TEvProposeTransaction::TPtr& ev) const {
    if (MvccSwitchState == TSwitchState::SWITCHING) {
        LOG_TRACE_S(*TlsActivationContext, NKikimrServices::TX_DATASHARD, "New transaction needs to wait because of mvcc state switching");
        return true;
    }

    auto &rec = ev->Get()->Record;
    if (rec.HasMvccSnapshot()) {
        TRowVersion rowVersion(rec.GetMvccSnapshot().GetStep(), rec.GetMvccSnapshot().GetTxId());
        TRowVersion unreadableEdge = Pipeline.GetUnreadableEdge();
        if (rowVersion >= unreadableEdge) {
            LOG_TRACE_S(*TlsActivationContext, NKikimrServices::TX_DATASHARD, "New transaction reads from " << rowVersion << " which is not before unreadable edge " << unreadableEdge);
            return true;
        }
    }

    return false;
}

bool TDataShard::CheckChangesQueueOverflow() const {
    const auto* appData = AppData();
    const auto sizeLimit = appData->DataShardConfig.GetChangesQueueItemsLimit();
    const auto bytesLimit = appData->DataShardConfig.GetChangesQueueBytesLimit();
    return ChangesQueue.size() >= sizeLimit || ChangesQueueBytes >= bytesLimit;
}

void TDataShard::Handle(TEvDataShard::TEvCancelTransactionProposal::TPtr &ev, const TActorContext &ctx) {
    ui64 txId = ev->Get()->Record.GetTxId();
    LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD, "Got TEvDataShard::TEvCancelTransactionProposal " << TabletID()
                << " txId " <<  txId);

    // Mark any queued proposals as cancelled
    ProposeQueue.Cancel(txId);

    // Cancel transactions that have already been proposed
    Execute(new TTxCancelTransactionProposal(this, txId), ctx);
}

void TDataShard::DoPeriodicTasks(const TActorContext &ctx) {
    UpdateLagCounters(ctx);
    UpdateTableStats(ctx);
    SendPeriodicTableStats(ctx);
    CollectCpuUsage(ctx);

    if (CurrentKeySampler == EnabledKeySampler && ctx.Now() > StopKeyAccessSamplingAt) {
        CurrentKeySampler = DisabledKeySampler;
        LOG_NOTICE_S(ctx, NKikimrServices::TX_DATASHARD, "Stoped key access sampling at datashard: " << TabletID());
    }

    ctx.Schedule(TDuration::Seconds(5), new TEvPrivate::TEvPeriodicWakeup());
}

void TDataShard::UpdateLagCounters(const TActorContext &ctx) {
    TDuration dataTxCompleteLag = GetDataTxCompleteLag();
    TabletCounters->Simple()[COUNTER_TX_COMPLETE_LAG].Set(dataTxCompleteLag.MilliSeconds());
    if (dataTxCompleteLag > TDuration::Minutes(5)) {
        LOG_WARN_S(ctx, NKikimrServices::TX_DATASHARD,
                   "Tx completion lag (" << dataTxCompleteLag << ") is > 5 min on tablet "
                   << TabletID());
    }

    TDuration scanTxCompleteLag = GetScanTxCompleteLag();
    TabletCounters->Simple()[COUNTER_SCAN_TX_COMPLETE_LAG].Set(scanTxCompleteLag.MilliSeconds());
    if (scanTxCompleteLag > TDuration::Hours(1)) {
        LOG_WARN_S(ctx, NKikimrServices::TX_DATASHARD,
                   "Scan completion lag (" << scanTxCompleteLag << ") is > 1 hour on tablet "
                   << TabletID());
    }
}

void TDataShard::FillSplitTrajectory(ui64 origin, NKikimrTx::TBalanceTrackList& tracks) {
    Y_UNUSED(origin);
    Y_UNUSED(tracks);
}

THolder<TEvTxProcessing::TEvReadSet>
TDataShard::PrepareReadSet(ui64 step, ui64 txId, ui64 source, ui64 target,
                                  const TString& body, ui64 seqno)
{
    auto ev = MakeHolder<TEvTxProcessing::TEvReadSet>(step, txId, source, target, TabletID(), body, seqno);
    if (source != TabletID())
        FillSplitTrajectory(source, *ev->Record.MutableBalanceTrackList());
    return ev;
}

void TDataShard::SendReadSet(const TActorContext& ctx, ui64 step,
                                    ui64 txId, ui64 source, ui64 target,
                                    const TString& body, ui64 seqno)
{
    LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD,
                "Send RS at " << TabletID() << " from " << source << " to " << target << " txId " << txId);

    auto ev = PrepareReadSet(step, txId, source, target, body, seqno);

    IncCounter(COUNTER_READSET_SENT_COUNT);
    IncCounter(COUNTER_READSET_SENT_SIZE, body.size());

    PipeClientCache->Send(ctx, target, ev.Release());
}

void TDataShard::SendReadSets(const TActorContext& ctx,
                                     TVector<THolder<TEvTxProcessing::TEvReadSet>> &&readsets)
{
    TPendingPipeTrackerCommands pendingPipeTrackerCommands;

    for (auto &rs : readsets) {
        ui64 txId = rs->Record.GetTxId();
        ui64 source = rs->Record.GetTabletSource();
        ui64 target = rs->Record.GetTabletDest();
        ui64 seqno = rs->Record.GetSeqno();

        LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD,
                    "Send RS at " << TabletID() << " from " << source
                    << " to " << target << " txId " << txId);

        IncCounter(COUNTER_READSET_SENT_COUNT);
        IncCounter(COUNTER_READSET_SENT_SIZE, rs->Record.GetReadSet().size());

        pendingPipeTrackerCommands.AttachTablet(seqno, target);
        PipeClientCache->Send(ctx, target, rs.Release());
    }

    pendingPipeTrackerCommands.Apply(ResendReadSetPipeTracker, ctx);
    readsets.clear();
}

void TDataShard::ResendReadSet(const TActorContext& ctx, ui64 step, ui64 txId, ui64 source, ui64 target,
                                      const TString& body, ui64 seqNo)
{
    LOG_INFO_S(ctx, NKikimrServices::TX_DATASHARD,
               "Resend RS at " << TabletID() << " from " << source << " to " << target << " txId " << txId);

    SendReadSet(ctx, step, txId, source, target, body, seqNo);
    ResendReadSetPipeTracker.AttachTablet(seqNo, target);
}

void TDataShard::UpdateLastSchemeOpSeqNo(const TSchemeOpSeqNo &newSeqNo,
                                                TTransactionContext &txc)
{
    NIceDb::TNiceDb db(txc.DB);
    if (LastSchemeOpSeqNo < newSeqNo) {
        LastSchemeOpSeqNo = newSeqNo;
        PersistSys(db, Schema::Sys_LastSchemeShardGeneration, LastSchemeOpSeqNo.Generation);
        PersistSys(db, Schema::Sys_LastSchemeShardRound, LastSchemeOpSeqNo.Round);
    }
}

void TDataShard::ResetLastSchemeOpSeqNo(TTransactionContext &txc)
{
    NIceDb::TNiceDb db(txc.DB);
    LastSchemeOpSeqNo = TSchemeOpSeqNo();
    PersistSys(db, Schema::Sys_LastSchemeShardGeneration, LastSchemeOpSeqNo.Generation);
    PersistSys(db, Schema::Sys_LastSchemeShardRound, LastSchemeOpSeqNo.Round);
}

void TDataShard::PersistProcessingParams(const NKikimrSubDomains::TProcessingParams &params,
                                                NTabletFlatExecutor::TTransactionContext &txc)
{
    NIceDb::TNiceDb db(txc.DB);
    ProcessingParams.reset(new NKikimrSubDomains::TProcessingParams());
    ProcessingParams->CopyFrom(params);
    PersistSys(db, TDataShard::Schema::Sys_SubDomainInfo,
               ProcessingParams->SerializeAsString());
}

void TDataShard::PersistCurrentSchemeShardId(ui64 id,
                                                   NTabletFlatExecutor::TTransactionContext &txc)
{
    NIceDb::TNiceDb db(txc.DB);
    CurrentSchemeShardId = id;
    PersistSys(db, TDataShard::Schema::Sys_CurrentSchemeShardId, id);
}

void TDataShard::PersistSubDomainPathId(ui64 ownerId, ui64 localPathId,
                                               NTabletFlatExecutor::TTransactionContext &txc)
{
    NIceDb::TNiceDb db(txc.DB);
    SubDomainPathId.emplace(ownerId, localPathId);
    PersistSys(db, Schema::Sys_SubDomainOwnerId, ownerId);
    PersistSys(db, Schema::Sys_SubDomainLocalPathId, localPathId);
}

void TDataShard::PersistOwnerPathId(ui64 id,
                                           NTabletFlatExecutor::TTransactionContext &txc)
{
    NIceDb::TNiceDb db(txc.DB);
    PathOwnerId = id;
    PersistSys(db, TDataShard::Schema::Sys_PathOwnerId, id);
}

void TDataShard::ResolveTablePath(const TActorContext &ctx)
{
    if (State != TShardState::Ready)
        return;

    for (auto &pr : TableInfos) {
        ui64 pathId = pr.first;
        const TUserTable &info = *pr.second;

        if (!info.Path) {
            if (!TableResolvePipe) {
                NTabletPipe::TClientConfig clientConfig;
                clientConfig.RetryPolicy = SchemeShardPipeRetryPolicy;
                TableResolvePipe = ctx.Register(NTabletPipe::CreateClient(ctx.SelfID, CurrentSchemeShardId, clientConfig));
            }

            auto *event = new TEvSchemeShard::TEvDescribeScheme(PathOwnerId,
                                                                    pathId);
            event->Record.MutableOptions()->SetReturnPartitioningInfo(false);
            event->Record.MutableOptions()->SetReturnPartitionConfig(false);
            event->Record.MutableOptions()->SetReturnChildren(false);
            NTabletPipe::SendData(ctx, TableResolvePipe, event);
        }
    }
}

void TDataShard::SerializeHistogram(const TUserTable &tinfo,
                                           const NTable::THistogram &histogram,
                                           const NScheme::TTypeRegistry &typeRegistry,
                                           NKikimrTxDataShard::TEvGetDataHistogramResponse::THistogram &hist)
{
    for (auto &item : histogram) {
        auto &rec = *hist.AddItems();
        rec.SetValue(item.Value);

        TSerializedCellVec key(item.EndKey);
        for (ui32 ki = 0; ki < tinfo.KeyColumnIds.size(); ++ki) {
            NScheme::ITypeSP t = typeRegistry.GetType(tinfo.KeyColumnTypes[ki]);
            DbgPrintValue(*rec.AddKeyValues(), key.GetCells()[ki], t.GetTypeId());
        }
    }
}

void TDataShard::SerializeKeySample(const TUserTable &tinfo,
                                           const NTable::TKeyAccessSample &keySample,
                                           const NScheme::TTypeRegistry &typeRegistry,
                                           NKikimrTxDataShard::TEvGetDataHistogramResponse::THistogram &hist)
{
    THashMap<TString, ui64> accessCounts;

    for (auto &key : keySample.GetSample()) {
        accessCounts[key.first]++;
        // TODO: count access kinds separately
    }

    for (auto &item : accessCounts) {
        auto &rec = *hist.AddItems();
        rec.SetValue(item.second);

        TSerializedCellVec key(item.first);
        for (ui32 ki = 0; ki < tinfo.KeyColumnIds.size() && ki < key.GetCells().size(); ++ki) {
            NScheme::ITypeSP t = typeRegistry.GetType(tinfo.KeyColumnTypes[ki]);
            DbgPrintValue(*rec.AddKeyValues(), key.GetCells()[ki], t.GetTypeId());
        }
    }
    Sort(hist.MutableItems()->begin(), hist.MutableItems()->end(),
         [] (const auto& a, const auto& b) { return a.GetValue() > b.GetValue(); });
}


void TDataShard::Handle(NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr ev,
                               const TActorContext &ctx)
{
    const auto &rec = ev->Get()->GetRecord();

    LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD,
                "Got scheme resolve result at " << TabletID() << ": "
                << rec.ShortDebugString());

    ui64 pathId = rec.GetPathId();
    if (!TableInfos.contains(pathId)) {
        LOG_ERROR_S(ctx, NKikimrServices::TX_DATASHARD,
                    "Shard " << TabletID() << " got describe result for unknown table "
                    << pathId);
        return;
    }

    if (!rec.GetPath()) {
        LOG_CRIT_S(ctx, NKikimrServices::TX_DATASHARD,
                   "Shard " << TabletID() << " couldn't get path for table "
                   << pathId << " with status " << rec.GetStatus());
        return;
    }
    Execute(new TTxStoreTablePath(this, pathId, rec.GetPath()), ctx);
}

void TDataShard::Handle(TEvDataShard::TEvGetInfoRequest::TPtr &ev,
                               const TActorContext &ctx)
{
    Execute(CreateTxGetInfo(this, ev), ctx);
}

void TDataShard::Handle(TEvDataShard::TEvListOperationsRequest::TPtr &ev,
                               const TActorContext &ctx)
{
    Execute(CreateTxListOperations(this, ev), ctx);
}

void TDataShard::Handle(TEvDataShard::TEvGetOperationRequest::TPtr &ev,
                               const TActorContext &ctx)
{
    Execute(CreateTxGetOperation(this, ev), ctx);
}

void TDataShard::Handle(TEvDataShard::TEvGetDataHistogramRequest::TPtr &ev,
                               const TActorContext &ctx)
{
    auto *response = new TEvDataShard::TEvGetDataHistogramResponse;
    response->Record.MutableStatus()->SetCode(Ydb::StatusIds::SUCCESS);
    const auto& rec = ev->Get()->Record; 

    if (rec.GetCollectKeySampleMs() > 0) { 
        EnableKeyAccessSampling(ctx, 
            AppData(ctx)->TimeProvider->Now() + TDuration::MilliSeconds(rec.GetCollectKeySampleMs())); 
    } 
 
    if (rec.GetActualData()) { 
        if (CurrentKeySampler == DisabledKeySampler) { 
            // datashard stores expired stats 
            ctx.Send(ev->Sender, response); 
            return; 
        } 
    } 
 
    auto &reg = *AppData(ctx)->TypeRegistry;
    for (const auto &pr : TableInfos) {
        const auto &tinfo = *pr.second;
        const NTable::TStats &stats = tinfo.Stats.DataStats;

        auto &hist = *response->Record.AddTableHistograms();
        hist.SetTableName(pr.second->Name);
        for (ui32 ki : tinfo.KeyColumnIds)
            hist.AddKeyNames(tinfo.Columns.FindPtr(ki)->Name);
        SerializeHistogram(tinfo, stats.DataSizeHistogram, reg, *hist.MutableSizeHistogram());
        SerializeHistogram(tinfo, stats.RowCountHistogram, reg, *hist.MutableCountHistogram());
        SerializeKeySample(tinfo, tinfo.Stats.AccessStats, reg, *hist.MutableKeyAccessSample());
    }

    ctx.Send(ev->Sender, response);
}

void TDataShard::Handle(TEvDataShard::TEvGetReadTableSinkStateRequest::TPtr &ev,
                               const TActorContext &ctx)
{
    ui64 txId = ev->Get()->Record.GetTxId();
    auto op = Pipeline.FindOp(txId);
    if (!op) {
        auto *response = new TEvDataShard::TEvGetReadTableSinkStateResponse;
        SetStatusError(response->Record, Ydb::StatusIds::NOT_FOUND,
                       TStringBuilder() << "Cannot find operation "
                       << txId << " on shard " << TabletID());
        ctx.Send(ev->Sender, response);
        return;
    }

    if (op->GetKind() != EOperationKind::ReadTable) {
        auto *response = new TEvDataShard::TEvGetReadTableSinkStateResponse;
        SetStatusError(response->Record, Ydb::StatusIds::BAD_REQUEST,
                       TStringBuilder() << "Cannot get sink state for tx of kind "
                       << op->GetKind());
        ctx.Send(ev->Sender, response);
        return;
    }

    TActiveTransaction *tx = dynamic_cast<TActiveTransaction*>(op.Get());
    Y_VERIFY_S(tx, "cannot cast operation of kind " << op->GetKind());
    ctx.Send(ev->Forward(tx->GetStreamSink()));
}

void TDataShard::Handle(TEvDataShard::TEvGetReadTableScanStateRequest::TPtr &ev,
                               const TActorContext &ctx)
{
    ui64 txId = ev->Get()->Record.GetTxId();
    auto op = Pipeline.FindOp(txId);
    if (!op) {
        auto *response = new TEvDataShard::TEvGetReadTableScanStateResponse;
        SetStatusError(response->Record, Ydb::StatusIds::NOT_FOUND,
                       TStringBuilder() << "Cannot find operation "
                       << txId << " on shard " << TabletID());
        ctx.Send(ev->Sender, response);
        return;
    }

    if (op->GetKind() != EOperationKind::ReadTable) {
        auto *response = new TEvDataShard::TEvGetReadTableScanStateResponse;
        SetStatusError(response->Record, Ydb::StatusIds::BAD_REQUEST,
                       TStringBuilder() << "Cannot get scan state for tx of kind "
                       << op->GetKind());
        ctx.Send(ev->Sender, response);
        return;
    }

    TActiveTransaction *tx = dynamic_cast<TActiveTransaction*>(op.Get());
    Y_VERIFY_S(tx, "cannot cast operation of kind " << op->GetKind());
    ctx.Send(ev->Forward(tx->GetStreamSink()));

    if (!tx->GetScanActor()) {
        auto *response = new TEvDataShard::TEvGetReadTableScanStateResponse;
        SetStatusError(response->Record, Ydb::StatusIds::GENERIC_ERROR,
                       TStringBuilder() << "Operation has no registered scan actor");
        ctx.Send(ev->Sender, response);
        return;
    }

    ctx.Send(ev->Forward(tx->GetScanActor()));
}

void TDataShard::Handle(TEvDataShard::TEvGetReadTableStreamStateRequest::TPtr &ev,
                               const TActorContext &ctx)
{
    ui64 txId = ev->Get()->Record.GetTxId();
    auto op = Pipeline.FindOp(txId);
    if (!op) {
        auto *response = new TEvDataShard::TEvGetReadTableStreamStateResponse;
        SetStatusError(response->Record, Ydb::StatusIds::NOT_FOUND,
                       TStringBuilder() << "Cannot find operation "
                       << txId << " on shard " << TabletID());
        ctx.Send(ev->Sender, response);
        return;
    }

    if (op->GetKind() != EOperationKind::ReadTable) {
        auto *response = new TEvDataShard::TEvGetReadTableStreamStateResponse;
        SetStatusError(response->Record, Ydb::StatusIds::BAD_REQUEST,
                       TStringBuilder() << "Cannot get stream state for tx of kind "
                       << op->GetKind());
        ctx.Send(ev->Sender, response);
        return;
    }

    TActiveTransaction *tx = dynamic_cast<TActiveTransaction*>(op.Get());
    Y_VERIFY_S(tx, "cannot cast operation of kind " << op->GetKind());
    ctx.Send(ev->Forward(tx->GetStreamSink()));
}

void TDataShard::Handle(TEvDataShard::TEvGetRSInfoRequest::TPtr &ev,
                               const TActorContext &ctx)
{
    auto *response = new TEvDataShard::TEvGetRSInfoResponse;
    response->Record.MutableStatus()->SetCode(Ydb::StatusIds::SUCCESS);

    for (auto &pr : OutReadSets.CurrentReadSets) {
        auto &rs = *response->Record.AddOutReadSets();
        rs.SetTxId(pr.second.TxId);
        rs.SetOrigin(pr.second.Origin);
        rs.SetSource(pr.second.From);
        rs.SetDestination(pr.second.To);
        rs.SetSeqNo(pr.first);
    }

    for (auto &p : OutReadSets.ReadSetAcks) {
        auto &rec = p->Record;
        auto &ack = *response->Record.AddOutRSAcks();
        ack.SetTxId(rec.GetTxId());
        ack.SetStep(rec.GetStep());
        ack.SetOrigin(rec.GetTabletConsumer());
        ack.SetSource(rec.GetTabletSource());
        ack.SetDestination(rec.GetTabletDest());
        ack.SetSeqNo(rec.GetSeqno());
    }

    for (auto &pr : Pipeline.GetDelayedAcks()) {
        auto *ev = dynamic_cast<TEvTxProcessing::TEvReadSetAck*>(pr.second->GetBase());
        if (ev) {
            auto &rec = ev->Record;
            auto &ack = *response->Record.AddDelayedRSAcks();
            ack.SetTxId(rec.GetTxId());
            ack.SetStep(rec.GetStep());
            ack.SetOrigin(rec.GetTabletConsumer());
            ack.SetSource(rec.GetTabletSource());
            ack.SetDestination(rec.GetTabletDest());
            ack.SetSeqNo(rec.GetSeqno());
        }
    }

    ctx.Send(ev->Sender, response);
}

void TDataShard::Handle(TEvDataShard::TEvGetSlowOpProfilesRequest::TPtr &ev,
                               const TActorContext &ctx)
{
    auto *response = new TEvDataShard::TEvGetSlowOpProfilesResponse;
    response->Record.MutableStatus()->SetCode(Ydb::StatusIds::SUCCESS);
    Pipeline.FillStoredExecutionProfiles(response->Record);
    ctx.Send(ev->Sender, response);
}

void TDataShard::Handle(TEvDataShard::TEvRefreshVolatileSnapshotRequest::TPtr& ev, const TActorContext& ctx) {
    Execute(new TTxRefreshVolatileSnapshot(this, std::move(ev)), ctx);
}

void TDataShard::Handle(TEvDataShard::TEvDiscardVolatileSnapshotRequest::TPtr& ev, const TActorContext& ctx) {
    Execute(new TTxDiscardVolatileSnapshot(this, std::move(ev)), ctx);
}

void TDataShard::Handle(TEvents::TEvUndelivered::TPtr &ev,
                               const TActorContext &ctx)
{
    auto op = Pipeline.FindOp(ev->Cookie);
    if (op) {
        op->AddInputEvent(ev.Release());
        Pipeline.AddCandidateOp(op);
        PlanQueue.Progress(ctx);
    }
}

void TDataShard::Handle(TEvInterconnect::TEvNodeDisconnected::TPtr &ev,
                               const TActorContext &ctx)
{
    const ui32 nodeId = ev->Get()->NodeId;

    LOG_NOTICE_S(ctx, NKikimrServices::TX_DATASHARD,
                 "Shard " << TabletID() << " disconnected from node " << nodeId);

    Pipeline.ProcessDisconnected(nodeId);
    PlanQueue.Progress(ctx);
}

void TDataShard::Handle(TEvDataShard::TEvMigrateSchemeShardRequest::TPtr& ev,
                               const TActorContext& ctx)
{
    Execute(new TTxMigrateSchemeShard(this, ev), ctx);
}

void TDataShard::Handle(TEvDataShard::TEvCancelBackup::TPtr& ev, const TActorContext& ctx)
{
    TOperation::TPtr op = Pipeline.FindOp(ev->Get()->Record.GetBackupTxId());
    if (op) {
        ForwardEventToOperation(ev, op, ctx);
    }
}

void TDataShard::Handle(TEvDataShard::TEvCancelRestore::TPtr& ev, const TActorContext& ctx)
{
    TOperation::TPtr op = Pipeline.FindOp(ev->Get()->Record.GetRestoreTxId());
    if (op) {
        ForwardEventToOperation(ev, op, ctx);
    }
}

void TDataShard::Handle(TEvDataShard::TEvGetS3Upload::TPtr& ev, const TActorContext& ctx)
{
    Execute(new TTxGetS3Upload(this, ev), ctx);
}

void TDataShard::Handle(TEvDataShard::TEvStoreS3UploadId::TPtr& ev, const TActorContext& ctx)
{
    Execute(new TTxStoreS3UploadId(this, ev), ctx);
}

void TDataShard::Handle(TEvDataShard::TEvChangeS3UploadStatus::TPtr& ev, const TActorContext& ctx)
{
    Execute(new TTxChangeS3UploadStatus(this, ev), ctx);
}

void TDataShard::Handle(TEvDataShard::TEvGetS3DownloadInfo::TPtr& ev, const TActorContext& ctx)
{
    Execute(new TTxGetS3DownloadInfo(this, ev), ctx);
}

void TDataShard::Handle(TEvDataShard::TEvStoreS3DownloadInfo::TPtr& ev, const TActorContext& ctx)
{
    Execute(new TTxStoreS3DownloadInfo(this, ev), ctx);
}

void TDataShard::Handle(TEvDataShard::TEvUnsafeUploadRowsRequest::TPtr& ev, const TActorContext& ctx)
{
    Execute(new TTxUnsafeUploadRows(this, ev), ctx);
}

void TDataShard::ScanComplete(NTable::EAbort,
                                     TAutoPtr<IDestructable> prod,
                                     ui64 cookie,
                                     const TActorContext &ctx)
{
    if (auto* noTxScan = dynamic_cast<INoTxScan*>(prod.Get())) {
        LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD, "Non-transactinal scan complete at "
                    << TabletID());

        noTxScan->OnFinished(this);
        prod.Destroy();
    } else {
        LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD,
                    "FullScan complete at " << TabletID());

        auto op = Pipeline.FindOp(cookie);
        if (op) {
            LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD, "Found op"
                << ": cookie: " << cookie
                << ", at: "<< TabletID());

            if (op->IsWaitingForScan()) {
                op->SetScanResult(prod);
                Pipeline.AddCandidateOp(op);
            }
        } else {
            if (InFlightCondErase && InFlightCondErase.TxId == cookie) {
                LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD, "Conditional erase complete"
                    << ": cookie: " << cookie
                    << ", at: "<< TabletID());

                InFlightCondErase.Clear();
            } else if (!Pipeline.FinishStreamingTx(cookie)) {
                LOG_ERROR_S(ctx, NKikimrServices::TX_DATASHARD,
                            "Scan complete at " << TabletID() << " for unknown tx " << cookie);
            }
        }
    }

    // Continue current Tx
    PlanQueue.Progress(ctx);
}

void TDataShard::Handle(TEvPrivate::TEvAsyncJobComplete::TPtr &ev, const TActorContext &ctx) {
    LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD, "AsyncJob complete"
        << " at " << TabletID());

    auto op = Pipeline.FindOp(ev->Cookie);
    if (op) {
        LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD, "Found op"
            << " at "<< TabletID()
            << " cookie " << ev->Cookie);

        if (op->IsWaitingForAsyncJob()) {
            op->SetAsyncJobResult(ev->Get()->Prod);
            Pipeline.AddCandidateOp(op);
        }
    } else {
        LOG_ERROR_S(ctx, NKikimrServices::TX_DATASHARD, "AsyncJob complete"
            << " at " << TabletID()
            << " for unknown tx " << ev->Cookie);
    }

    // Continue current Tx
    PlanQueue.Progress(ctx);
}

bool TDataShard::ReassignChannelsEnabled() const {
    return true;
}

void TDataShard::ExecuteProgressTx(const TActorContext& ctx) {
    Execute(new TTxProgressTransaction(this), ctx);
}

void TDataShard::ExecuteProgressTx(TOperation::TPtr op, const TActorContext& ctx) {
    Y_VERIFY(op->IsInProgress());
    Execute(new TTxProgressTransaction(this, std::move(op)), ctx);
}

TDuration TDataShard::CleanupTimeout() const {
    const TDuration pipelineTimeout = Pipeline.CleanupTimeout();
    const TDuration snapshotTimeout = SnapshotManager.CleanupTimeout();
    const TDuration minTimeout = TDuration::Seconds(1);
    const TDuration maxTimeout = TDuration::MilliSeconds(DefaultTxStepDeadline() / 2);
    return Max(minTimeout, Min(pipelineTimeout, snapshotTimeout, maxTimeout));
}

class TDataShard::TTxGetRemovedRowVersions : public NTabletFlatExecutor::TTransactionBase<TDataShard> {
public:
    TTxGetRemovedRowVersions(TDataShard* self, TEvDataShard::TEvGetRemovedRowVersions::TPtr&& ev)
        : TTransactionBase(self)
        , Ev(std::move(ev))
    { }

    bool Execute(TTransactionContext& txc, const TActorContext&) override {
        auto pathId = Ev->Get()->PathId;
        auto it = pathId ? Self->GetUserTables().find(pathId.LocalPathId) : Self->GetUserTables().begin();
        Y_VERIFY(it != Self->GetUserTables().end());

        Reply = MakeHolder<TEvDataShard::TEvGetRemovedRowVersionsResult>(txc.DB.GetRemovedRowVersions(it->second->LocalTid));
        return true;
    }

    void Complete(const TActorContext& ctx) override {
        ctx.Send(Ev->Sender, Reply.Release(), 0, Ev->Cookie);
    }

private:
    TEvDataShard::TEvGetRemovedRowVersions::TPtr Ev;
    THolder<TEvDataShard::TEvGetRemovedRowVersionsResult> Reply;
};

void TDataShard::Handle(TEvDataShard::TEvGetRemovedRowVersions::TPtr& ev, const TActorContext& ctx) {
    Execute(new TTxGetRemovedRowVersions(this, std::move(ev)), ctx);
}

} // NDataShard

TString TEvDataShard::TEvRead::ToString() const {
    TStringStream ss;
    ss << TBase::ToString();
    if (!Keys.empty()) {
        ss << " KeysSize: " << Keys.size();
    }
    if (!Ranges.empty()) {
        ss << " RangesSize: " << Ranges.size();
    }
    return ss.Str();
}

NActors::IEventBase* TEvDataShard::TEvRead::Load(TEventSerializedData* data) {
    auto* base = TBase::Load(data);
    auto* event = static_cast<TEvRead*>(base);
    auto& record = event->Record;

    event->Keys.reserve(record.KeysSize());
    for (const auto& key: record.GetKeys()) {
        event->Keys.emplace_back(key);
    }

    event->Ranges.reserve(record.RangesSize());
    for (const auto& range: record.GetRanges()) {
        event->Ranges.emplace_back(range);
    }

    return base;
}

// really ugly hacky, because Record is not mutable and calling members are const
void TEvDataShard::TEvRead::FillRecord() {
    if (!Keys.empty()) {
        Record.MutableKeys()->Reserve(Keys.size());
        for (auto& key: Keys) {
            Record.AddKeys(key.ReleaseBuffer());
        }
        Keys.clear();
    }

    if (!Ranges.empty()) {
        Record.MutableRanges()->Reserve(Ranges.size());
        for (auto& range: Ranges) {
            auto* pbRange = Record.AddRanges();
            range.Serialize(*pbRange);
        }
        Ranges.clear();
    }
}

TString TEvDataShard::TEvReadResult::ToString() const {
    TStringStream ss;
    ss << TBase::ToString();

    if (ArrowBatch) {
        ss << " ArrowRows: " << ArrowBatch->num_rows()
           << " ArrowCols: " << ArrowBatch->num_columns();
    }

    if (!Rows.empty()) {
        ss << " RowsSize: " << Rows.size();
    }

    return ss.Str();
}

NActors::IEventBase* TEvDataShard::TEvReadResult::Load(TEventSerializedData* data) {
    auto* base = TBase::Load(data);
    auto* event = static_cast<TEvReadResult*>(base);
    auto& record = event->Record;

    if (record.HasArrowBatch()) {
        const auto& batch = record.GetArrowBatch();
        auto schema = NArrow::DeserializeSchema(batch.GetSchema());
        event->ArrowBatch = NArrow::DeserializeBatch(batch.GetBatch(), schema);
        record.ClearArrowBatch();
    } else if (record.HasCellVec()) {
        auto& batch = *record.MutableCellVec();
        event->RowsSerialized.reserve(batch.RowsSize());
        for (auto& row: *batch.MutableRows()) {
            event->RowsSerialized.emplace_back(std::move(row));
        }
        record.ClearCellVec();
    }

    return base;
}

void TEvDataShard::TEvReadResult::FillRecord() {
    if (ArrowBatch) {
        auto* protoBatch = Record.MutableArrowBatch();
        protoBatch->SetSchema(NArrow::SerializeSchema(*ArrowBatch->schema()));
        protoBatch->SetBatch(NArrow::SerializeBatchNoCompression(ArrowBatch));
        ArrowBatch.reset();
        return;
    } else if (!Rows.empty()) {
        auto* protoBatch = Record.MutableCellVec();
        protoBatch->MutableRows()->Reserve(Rows.size());
        for (const auto& row: Rows) {
            protoBatch->AddRows(TSerializedCellVec::Serialize(row));
        }
        Rows.clear();
        return;
    }
}

} // NKikimr
