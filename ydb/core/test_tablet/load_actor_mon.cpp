#include "load_actor_impl.h"

namespace NKikimr::NTestShard {

    void TLoadActor::Handle(NMon::TEvRemoteHttpInfo::TPtr ev) {
        class TQueryProcessorActor : public TActorBootstrapped<TQueryProcessorActor> {
            NMon::TEvRemoteHttpInfo::TPtr Ev;
            const TActorId ValidationActorId;
            TString Html;
            TString ValidationHtml;

        public:
            TQueryProcessorActor(NMon::TEvRemoteHttpInfo::TPtr ev, TLoadActor *self)
                : Ev(ev)
                , ValidationActorId(self->ValidationActorId)
            {
                Html = RenderHtml(self);
            }

            void Bootstrap() {
                Become(&TThis::StateFunc);
                if (ValidationActorId) {
                    Send(ValidationActorId, new NMon::TEvRemoteHttpInfo(Ev->Get()->Query));
                } else {
                    PassAway();
                }
            }

            void Handle(NMon::TEvRemoteHttpInfoRes::TPtr ev) {
                ValidationHtml = ev->Get()->Html;
                PassAway();
            }

            TString RenderHtml(TLoadActor *self) {
                const TInstant now = TActivationContext::Now();
                const TCgiParameters& params = Ev->Get()->Cgi();
                TStringStream str;
                HTML(str) {
                    DIV_CLASS("panel panel-info") {
                        DIV_CLASS("panel-heading") {
                            str << "Load Actor";
                        }
                        DIV_CLASS("panel-body") {
                            TABLE_CLASS("table table-condensed") {
                                TABLEHEAD() {
                                    TABLER() {
                                        TABLEH() { str << "Parameter"; }
                                        TABLEH() { str << "Value"; }
                                    }
                                }
                                TABLEBODY() {
                                    TABLER() {
                                        TABLED() { str << "Tablet id"; }
                                        TABLED() { str << self->TabletId; }
                                    }

                                    TABLER() {
                                        TABLED() { str << "Generation"; }
                                        TABLED() { str << self->Generation; }
                                    }

                                    TABLER() {
                                        size_t numPoints = 0;
                                        TDuration timeSpan;
                                        const ui64 speed = self->WriteSpeed.GetSpeedInBytesPerSecond(now, &numPoints, &timeSpan);
                                        TABLED() { str << "Write speed"; }
                                        TABLED() { str << Sprintf("%.2lf", speed * 1e-6) << " MB/s @" << numPoints << ":" << timeSpan; }
                                    }

                                    TABLER() {
                                        TABLED() { str << "Bytes of data stored"; }
                                        TABLED() { str << self->BytesOfData; }
                                    }

                                    TABLER() {
                                        TABLED() { str << "Bytes of trash stored"; }
                                        TABLED() { str << params.Get("trashvol"); }
                                    }

                                    TABLER() {
                                        TABLED() { str << "Bytes processed"; }
                                        TABLED() { str << self->BytesProcessed; }
                                    }

                                    TABLER() {
                                        TABLED() { str << "Bytes written"; }
                                        TABLED() { str << self->WriteSpeed.GetBytesAccum(); }
                                    }

                                    TABLER() {
                                        TABLED() { str << "Keys written"; }
                                        TABLED() { str << self->KeysWritten; }
                                    }

                                    TABLER() {
                                        TABLED() { str << "Keys deleted"; }
                                        TABLED() { str << self->KeysDeleted; }
                                    }

                                    TABLER() {
                                        TABLED() { str << "Writes in flight"; }
                                        TABLED() { str << self->WritesInFlight.size(); }
                                    }

                                    TABLER() {
                                        TABLED() { str << "Delete requests in flight"; }
                                        TABLED() { str << self->DeletesInFlight.size(); }
                                    }

                                    TABLER() {
                                        size_t num = 0;
                                        for (const auto& [cookie, item] : self->DeletesInFlight) {
                                            num += item.KeysInQuery.size();
                                        }
                                        TABLED() { str << "Delete keys in flight"; }
                                        TABLED() { str << num; }
                                    }

                                    TABLER() {
                                        TABLED() { str << "Count of validation runnings"; }
                                        TABLED() { str << self->ValidationRunningCount; }
                                    }

                                    const std::vector<double> ps{0.0, 0.5, 0.9, 0.99, 1.0};

                                    auto output = [&](auto& r, const char *name) {
                                        auto res = r.Percentiles(ps);
                                        for (size_t i = 0; i < ps.size(); ++i) {
                                            TABLER() {
                                                TABLED() { str << (i ? "" : name); }
                                                TABLED() { str << Sprintf("%4.2lf", ps[i]) << "# " << res[i]; }
                                            }
                                        }
                                    };

                                    output(self->StateServerWriteLatency, "StateServerWriteLatency");
                                    output(self->WriteLatency, "WriteLatency");
                                }
                            }
                        }
                    }
                }
                return str.Str();
            }

            void PassAway() override {
                Send(Ev->Sender, new NMon::TEvRemoteHttpInfoRes(Html + ValidationHtml), 0, Ev->Cookie);
                TActorBootstrapped::PassAway();
            }

            STRICT_STFUNC(StateFunc,
                hFunc(NMon::TEvRemoteHttpInfoRes, Handle);
            )
        };

        Register(new TQueryProcessorActor(ev, this));
    }

} // NKikimr::NTestShard
