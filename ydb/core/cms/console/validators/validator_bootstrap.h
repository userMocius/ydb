#pragma once

#include "validator.h"

namespace NKikimr {
namespace NConsole {

class TBootstrapConfigValidator : public IConfigValidator {
public:
    TBootstrapConfigValidator();

    TString GetDescription() const override;
    bool CheckConfig(const NKikimrConfig::TAppConfig &oldConfig,
                     const NKikimrConfig::TAppConfig &newConfig,
                     TVector<Ydb::Issue::IssueMessage> &issues) const override;

private:
    bool CheckTablets(const NKikimrConfig::TAppConfig &config,
                      TVector<Ydb::Issue::IssueMessage> &issues) const;
    bool CheckResourceBrokerConfig(const NKikimrConfig::TAppConfig &config,
                                   TVector<Ydb::Issue::IssueMessage> &issues) const;
    bool CheckResourceBrokerOverrides(const NKikimrConfig::TAppConfig &config,
                                      TVector<Ydb::Issue::IssueMessage> &issues) const;
    bool IsUnlimitedResource(const NKikimrResourceBroker::TResources &limit) const;
};

} // namespace NConsole
} // namespace NKikimr
