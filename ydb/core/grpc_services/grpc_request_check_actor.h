#pragma once
#include "defs.h"
#include "grpc_proxy_counters.h"
#include "local_rate_limiter.h"
#include "operation_helpers.h"
#include "rpc_calls.h"

#include <library/cpp/actors/core/actor_bootstrapped.h>

#include <ydb/core/base/path.h>
#include <ydb/core/base/subdomain.h>
#include <ydb/core/base/kikimr_issue.h>
#include <ydb/core/security/secure_request.h>
#include <ydb/core/tx/scheme_cache/scheme_cache.h>

#include <util/string/split.h>

namespace NKikimr {
namespace NGRpcService {

template <typename TEvent>
class TGrpcRequestCheckActor
    : public TActorBootstrappedSecureRequest<TGrpcRequestCheckActor<TEvent>>
    , public ICheckerIface
{
    using TSelf = TGrpcRequestCheckActor<TEvent>;
    using TBase = TActorBootstrappedSecureRequest<TGrpcRequestCheckActor>;
public:
    void OnAccessDenied(const TEvTicketParser::TError& error, const TActorContext& ctx) {
        LOG_ERROR(ctx, NKikimrServices::GRPC_SERVER, error.ToString());
        if (error.Retryable) {
            GrpcRequestBaseCtx_->UpdateAuthState(NGrpc::TAuthState::AS_UNAVAILABLE);
        } else {
            GrpcRequestBaseCtx_->UpdateAuthState(NGrpc::TAuthState::AS_FAIL);
        }
        ReplyBackAndDie();
    }

    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::GRPC_REQ_AUTH;
    }

    static const TVector<TString>& GetPermissions();

    void InitializeAttributesFromSchema(const TSchemeBoardEvents::TDescribeSchemeResult& schemeData) {
        CheckedDatabaseName_ = CanonizePath(schemeData.GetPath());
        if (!GrpcRequestBaseCtx_->TryCustomAttributeProcess(schemeData, this)) {
            ProcessCommonAttributes(schemeData);
        }
    }

    void ProcessCommonAttributes(const TSchemeBoardEvents::TDescribeSchemeResult& schemeData) {
        static std::vector<TString> allowedAttributes = {"folder_id", "service_account_id", "database_id"};
        TVector<std::pair<TString, TString>> attributes;
        attributes.reserve(schemeData.GetPathDescription().UserAttributesSize());
        for (const auto& attr : schemeData.GetPathDescription().GetUserAttributes()) {
            if (std::find(allowedAttributes.begin(), allowedAttributes.end(), attr.GetKey()) != allowedAttributes.end()) {
                attributes.emplace_back(attr.GetKey(), attr.GetValue());
            }
        }
        if (!attributes.empty()) {
            SetEntries({{GetPermissions(), attributes}});
        }
    }

    void SetEntries(const TVector<TEvTicketParser::TEvAuthorizeTicket::TEntry>& entries) {
        TBase::SetEntries(entries);
    }

    void InitializeAttributes(const TSchemeBoardEvents::TDescribeSchemeResult& schemeData);

    TGrpcRequestCheckActor(
        const TActorId& owner,
        const TSchemeBoardEvents::TDescribeSchemeResult& schemeData,
        TIntrusivePtr<TSecurityObject> securityObject,
        TAutoPtr<TEventHandle<TEvent>> request,
        TGrpcProxyCounters::TPtr counters)
        : Owner_(owner)
        , Request_(std::move(request))
        , Counters_(counters)
        , SecurityObject_(std::move(securityObject))
    {
        GrpcRequestBaseCtx_ = Request_->Get();
        TMaybe<TString> authToken = GrpcRequestBaseCtx_->GetYdbToken();
        if (authToken) {
            TString peerName = GrpcRequestBaseCtx_->GetPeerName();
            TBase::SetSecurityToken(authToken.GetRef());
            TBase::SetPeerName(peerName);
            InitializeAttributes(schemeData);
            TBase::SetDatabase(CheckedDatabaseName_);
        }
    }

    void Bootstrap(const TActorContext& ctx) {
        TBase::Become(&TSelf::DbAccessStateFunc);

        if (SecurityObject_) {
            const ui32 access = NACLib::ConnectDatabase;
            if (!SecurityObject_->CheckAccess(access, TBase::GetSerializedToken())) {
                const TString error = TStringBuilder()
                    << "User has no permission to perform query on this database"
                    << ", database: " << CheckedDatabaseName_
                    << ", user: " << TBase::GetUserSID()
                    << ", from ip: " << GrpcRequestBaseCtx_->GetPeerName();
                LOG_INFO(*TlsActivationContext, NKikimrServices::GRPC_PROXY_NO_CONNECT_ACCESS, "%s", error.c_str());

                Counters_->IncDatabaseAccessDenyCounter();
                if (AppData()->FeatureFlags.GetCheckDatabaseAccessPermission()) {
                    LOG_ERROR(*TlsActivationContext, NKikimrServices::GRPC_SERVER, "%s", error.c_str());
                    ReplyUnauthorizedAndDie(MakeIssue(NKikimrIssues::TIssuesIds::ACCESS_DENIED, error));
                    return;
                }
            }
        }

        // Simple rps limitation
        static NRpcService::TRlConfig rpsRlConfig(
            "serverless_rt_coordination_node_path",
            "serverless_rt_base_resource_rps",
                {
                    NRpcService::TRlConfig::TOnReqAction {
                        1
                    }
                }
            );

        // Limitation RU for unary calls in time of response
        static NRpcService::TRlConfig ruRlConfig(
            "serverless_rt_coordination_node_path",
            "serverless_rt_base_resource_ru",
                {
                    NRpcService::TRlConfig::TOnReqAction {
                        1
                    },
                    NRpcService::TRlConfig::TOnRespAction {
                    }
                }
            );

        // Limitation ru for calls with internall rl support (read table)
        static NRpcService::TRlConfig ruRlProgressConfig(
            "serverless_rt_coordination_node_path",
            "serverless_rt_base_resource_ru",
                {
                    NRpcService::TRlConfig::TOnReqAction {
                        1
                    }
                }
            );


        auto rlMode = Request_->Get()->GetRlMode();
        switch (rlMode) {
            case TRateLimiterMode::Rps:
                RlConfig = &rpsRlConfig;
                break;
            case TRateLimiterMode::Ru:
                RlConfig = &ruRlConfig;
                break;
            case TRateLimiterMode::RuOnProgress:
                RlConfig = &ruRlProgressConfig;
                break;
            case TRateLimiterMode::Off:
                break;
        }

        if (!RlConfig) {
            // No rate limit config for this request
            return SetTokenAndDie(CheckedDatabaseName_);
        } else {
            // TODO(xenoxeno): get rid of unnecessary hash map
            THashMap<TString, TString> attributes;
            for (const auto& entry : TBase::GetEntries()) {
                for (const auto& [attrName, attrValue] : entry.Attributes) {
                    attributes[attrName] = attrValue;
                }
            }
            return ProcessRateLimit(attributes, ctx);
        }
    }

    void SetTokenAndDie(const TString& database = {}) {
        GrpcRequestBaseCtx_->UpdateAuthState(NGrpc::TAuthState::AS_OK);
        GrpcRequestBaseCtx_->SetInternalToken(TBase::GetSerializedToken());
        GrpcRequestBaseCtx_->UseDatabase(database);
        ReplyBackAndDie();
    }

    void SetRlPath(TMaybe<NRpcService::TRlPath>&& rlPath) {
        GrpcRequestBaseCtx_->SetRlPath(std::move(rlPath));
    }

    STATEFN(DbAccessStateFunc) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvents::TEvPoisonPill, HandlePoison);
        }
    }

    void HandlePoison(TEvents::TEvPoisonPill::TPtr&) {
        TBase::PassAway();
    }

private:
    static NYql::TIssues GetRlIssues(const Ydb::RateLimiter::AcquireResourceResponse& resp) {
        NYql::TIssues opIssues;
        NYql::IssuesFromMessage(resp.operation().issues(), opIssues);
        return opIssues;
    }

    void ProcessOnRequest(Ydb::RateLimiter::AcquireResourceRequest&& req, const TActorContext& ctx) {
        auto time = TInstant::Now();
        auto cb = [this, time](Ydb::RateLimiter::AcquireResourceResponse resp) {
            TDuration delay = TInstant::Now() - time;
            switch (resp.operation().status()) {
                case Ydb::StatusIds::SUCCESS:
                    LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::GRPC_SERVER, "Request delayed for " << delay << " by ratelimiter");
                    SetTokenAndDie(CheckedDatabaseName_);
                    break;
                case Ydb::StatusIds::TIMEOUT:
                    Counters_->IncDatabaseRateLimitedCounter();
                    LOG_ERROR(*TlsActivationContext, NKikimrServices::GRPC_SERVER, "Throughput limit exceeded");
                    ReplyOverloadedAndDie(MakeIssue(NKikimrIssues::TIssuesIds::YDB_RESOURCE_USAGE_LIMITED, "Throughput limit exceeded"));
                    break;
                default:
                    {
                        auto issues = GetRlIssues(resp);
                        const TString error = Sprintf("RateLimiter status: %d database: %s, issues: %s",
                                              resp.operation().status(),
                                              CheckedDatabaseName_.c_str(),
                                              issues.ToString().c_str());
                        LOG_ERROR(*TlsActivationContext, NKikimrServices::GRPC_SERVER, "%s", error.c_str());

                        ReplyUnavailableAndDie(issues); // same as cloud-go serverless proxy
                    }
                    break;
            }
        };

        req.mutable_operation_params()->mutable_operation_timeout()->set_nanos(200000000); // same as cloud-go serverless proxy

        NKikimr::NRpcService::RateLimiterAcquireUseSameMailbox(
            std::move(req),
            CheckedDatabaseName_,
            TBase::GetSerializedToken(),
            std::move(cb),
            ctx);
    }

    TRespHook CreateRlRespHook(Ydb::RateLimiter::AcquireResourceRequest&& req) {
        const auto& databasename = CheckedDatabaseName_;
        auto token = TBase::GetSerializedToken();
        return [req{std::move(req)}, databasename, token](TRespHookCtx::TPtr ctx) mutable {

            LOG_DEBUG(*TlsActivationContext, NKikimrServices::GRPC_SERVER,
                "Response hook called to report RU usage, database: %s, request: %s, consumed: %d",
                databasename.c_str(), ctx->GetRequestName().c_str(), ctx->GetConsumedRu());

            if (ctx->GetConsumedRu() >= 1) {
                // We already count '1' on start request
                req.set_used(ctx->GetConsumedRu() - 1);

                // No need to handle result of rate limiter response on the response hook
                // just report ru usage
                auto noop = [](Ydb::RateLimiter::AcquireResourceResponse) {};
                NKikimr::NRpcService::RateLimiterAcquireUseSameMailbox(
                    std::move(req),
                    databasename,
                    token,
                    std::move(noop),
                    TActivationContext::AsActorContext());
            }

            ctx->Pass();
        };
    }

    void ProcessRateLimit(const THashMap<TString, TString>& attributes, const TActorContext& ctx) {
        // Match rate limit config and database attributes
        auto rlPath = NRpcService::Match(*RlConfig, attributes);
        if (!rlPath) {
            return SetTokenAndDie(CheckedDatabaseName_);
        } else {
            auto actions = NRpcService::MakeRequests(*RlConfig, rlPath.GetRef());
            SetRlPath(std::move(rlPath));

            Ydb::RateLimiter::AcquireResourceRequest req;
            bool hasOnReqAction = false;
            for (auto& action : actions) {
                switch (action.first) {
                case NRpcService::Actions::OnReq:
                    req = std::move(action.second);
                    hasOnReqAction = true;
                    break;
                case NRpcService::Actions::OnResp:
                    GrpcRequestBaseCtx_->SetRespHook(CreateRlRespHook(std::move(action.second)));
                    break;
                }
            }

            if (hasOnReqAction) {
                return ProcessOnRequest(std::move(req), ctx);
            } else {
                return SetTokenAndDie(CheckedDatabaseName_);
            }
        }
    }

private:
    void ReplyUnauthorizedAndDie(const NYql::TIssue& issue) {
        GrpcRequestBaseCtx_->RaiseIssue(issue);
        GrpcRequestBaseCtx_->ReplyWithYdbStatus(Ydb::StatusIds::UNAUTHORIZED);
        TBase::PassAway();
    }

    void ReplyUnavailableAndDie(const NYql::TIssue& issue) {
        GrpcRequestBaseCtx_->RaiseIssue(issue);
        GrpcRequestBaseCtx_->ReplyWithYdbStatus(Ydb::StatusIds::UNAVAILABLE);
        TBase::PassAway();
    }

    void ReplyUnavailableAndDie(const NYql::TIssues& issue) {
        GrpcRequestBaseCtx_->RaiseIssues(issue);
        GrpcRequestBaseCtx_->ReplyWithYdbStatus(Ydb::StatusIds::UNAVAILABLE);
        TBase::PassAway();
    }

    void ReplyUnauthenticatedAndDie() {
        GrpcRequestBaseCtx_->ReplyUnauthenticated("Unknown database");
        TBase::PassAway();
    }

    void ReplyOverloadedAndDie(const NYql::TIssue& issue) {
        GrpcRequestBaseCtx_->RaiseIssue(issue);
        GrpcRequestBaseCtx_->ReplyWithYdbStatus(Ydb::StatusIds::OVERLOADED);
        TBase::PassAway();
    }

    void ReplyBackAndDie() {
        TlsActivationContext->Send(Request_->Forward(Owner_));
        TBase::PassAway();
    }

    const TActorId Owner_;
    TAutoPtr<TEventHandle<TEvent>> Request_;
    TGrpcProxyCounters::TPtr Counters_;
    TIntrusivePtr<TSecurityObject> SecurityObject_;
    TString CheckedDatabaseName_;
    IRequestProxyCtx* GrpcRequestBaseCtx_;
    NRpcService::TRlConfig* RlConfig = nullptr;
};

// default behavior - attributes in schema
template <typename TEvent>
void TGrpcRequestCheckActor<TEvent>::InitializeAttributes(const TSchemeBoardEvents::TDescribeSchemeResult& schemeData) {
    InitializeAttributesFromSchema(schemeData);
}

// default permissions
template <typename TEvent>
const TVector<TString>& TGrpcRequestCheckActor<TEvent>::GetPermissions() {
    static const TVector<TString> permissions = {
                "ydb.databases.list",
                "ydb.databases.create",
                "ydb.databases.connect"
            };
    return permissions;
}

// yds behavior
template <>
inline const TVector<TString>& TGrpcRequestCheckActor<TEvDataStreamsPutRecordRequest>::GetPermissions() {
    //full list of permissions for compatility. remove old permissions later.
    static const TVector<TString> permissions = {"yds.streams.write", "ydb.databases.list", "ydb.databases.create", "ydb.databases.connect"};
    return permissions;
}
// yds behavior
template <>
inline const TVector<TString>& TGrpcRequestCheckActor<TEvDataStreamsPutRecordsRequest>::GetPermissions() {
    //full list of permissions for compatility. remove old permissions later.
    static const TVector<TString> permissions = {"yds.streams.write", "ydb.databases.list", "ydb.databases.create", "ydb.databases.connect"};
    return permissions;
}

template <typename TEvent>
IActor* CreateGrpcRequestCheckActor(
    const TActorId& owner,
    const TSchemeBoardEvents::TDescribeSchemeResult& schemeData,
    TIntrusivePtr<TSecurityObject> securityObject,
    TAutoPtr<TEventHandle<TEvent>> request,
    TGrpcProxyCounters::TPtr counters) {

    return new TGrpcRequestCheckActor<TEvent>(owner, schemeData, std::move(securityObject), std::move(request), counters);
}

}
}
