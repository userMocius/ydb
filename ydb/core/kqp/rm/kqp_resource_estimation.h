#pragma once

#include <ydb/core/protos/config.pb.h>
#include <ydb/core/kqp/runtime/kqp_scan_data.h>

#include <ydb/library/yql/dq/proto/dq_tasks.pb.h>


namespace NKikimr::NKqp {

struct TTaskResourceEstimation {
    ui64 TaskId = 0;
    ui32 ScanBuffersCount = 0;
    ui32 ChannelBuffersCount = 0;
    ui64 ScanBufferMemoryLimit = 0;
    ui64 ChannelBufferMemoryLimit = 0;
    ui64 MkqlProgramMemoryLimit = 0;
    ui64 TotalMemoryLimit = 0;

    TString ToString() const {
        return TStringBuilder() << "TaskResourceEstimation{"
            << " TaskId: " << TaskId
            << ", ScanBuffersCount: " << ScanBuffersCount
            << ", ChannelBuffersCount: " << ChannelBuffersCount
            << ", ScanBufferMemoryLimit: " << ScanBufferMemoryLimit
            << ", ChannelBufferMemoryLimit: " << ChannelBufferMemoryLimit
            << ", MkqlProgramMemoryLimit: " << MkqlProgramMemoryLimit
            << ", TotalMemoryLimit: " << TotalMemoryLimit
            << " }";
    }
};

TTaskResourceEstimation EstimateTaskResources(const NYql::NDqProto::TDqTask& task, int nScans, ui32 dsOnNodeCount, 
    const NKikimrConfig::TTableServiceConfig::TResourceManager& config);

void EstimateTaskResources(const NYql::NDqProto::TDqTask& task, int nScans, ui32 dsOnNodeCount,
    const NKikimrConfig::TTableServiceConfig::TResourceManager& config, TTaskResourceEstimation& result);

TVector<TTaskResourceEstimation> EstimateTasksResources(const TVector<NYql::NDqProto::TDqTask>& tasks, int nScans,
    ui32 dsOnNodeCount, const NKikimrConfig::TTableServiceConfig::TResourceManager& config);

} // namespace NKikimr::NKqp
