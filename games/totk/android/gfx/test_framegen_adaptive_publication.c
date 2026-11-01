/* MIT. Deterministic production publication-audit helpers; no Android, GPU,
 * clocks, sleeps or resource retirement. Actual worker/snapshot ownership is
 * covered by test_present_retire.c. These scalar queue snapshots deliberately
 * keep retired GEN history while replacing its live transaction identity.
 * Build/run through the parent's dedicated host-test target. */
#include "framegen_adaptive_publication.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(condition, message) do { checks++; if (!(condition)) { \
    fprintf(stderr,"adaptive publication: %s (line %d)\n",message,__LINE__); exit(1); \
} } while (0)
static const uint64_t second=UINT64_C(1000000000);
static const uint64_t period=UINT64_C(10000000);
static const uint64_t margin=UINT64_C(2000000);

static fgs_request prepare_generation(fgs_state *state) {
    CHECK(fgs_init(state,7,second+period,period,margin)==FGS_OK,"production lattice initializes");
    fgs_batch batch={0};
    for (unsigned frame=0;frame<=FGS_WARMUP_INTERVALS;frame++) {
        uint64_t now=second+(uint64_t)frame*3*period;
        fgs_real_plan real;
        CHECK(fgs_peek_real(state,now,&real)==FGS_OK,"production warmup plans REAL");
        CHECK(fgs_commit_real(state,real.slot,now,frame+1,3,1,&batch)==FGS_OK,
            "production REAL publication trains cadence");
    }
    CHECK(batch.request_count>0,"causal warmup produces a real GEN request");
    fgs_request request=batch.requests[0];
    fgs_cancel cancelled;
    CHECK(fgs_mark_ready(state,request.token,request.deadline_ns-1,&cancelled)==FGS_OK,
        "GEN backend readiness precedes its cutoff");
    fgs_request admitted;
    CHECK(fgs_admit_gen(state,request.token,request.slot.target_ns-2,NULL,&admitted)==FGS_OK,
        "production admission permits late pre-target GEN apply");
    return admitted;
}
static fgs_applied_generation applied(fgs_request request, unsigned index,
        uint64_t sequence, uint64_t apply_ns) {
    return (fgs_applied_generation){.id={index,sequence,request.slot.window_epoch},
        .slot=request.slot,.source_frame=request.frame,.source_epoch=request.source_epoch,
        .apply_ns=apply_ns};
}
static fgs_real_observation newer_real(fgs_applied_generation generation, uint64_t ready,
        uint64_t attempt) {
    return (fgs_real_observation){.frame=generation.source_frame+1,
        .source_epoch=generation.source_epoch,.window_epoch=generation.slot.window_epoch,
        .ready_ns=ready,.first_attempt_ns=attempt,.ready_time_known=1};
}
static void late_real_does_not_redefine_occupied_conflicts(void) {
    fgs_state state;
    fgs_request request=prepare_generation(&state);
    uint64_t target=request.slot.target_ns;
    fgs_gen_commit committed;
    CHECK(fgs_commit_gen(&state,request.token,target-2,&committed)==FGS_OK,
        "actual late GEN apply is irreversibly committed");
    CHECK(committed.deadline_missed && !committed.target_missed,
        "S-2 apply misses cutoff but not target");
    fgs_publication_history history={0};
    fgs_applied_generation generation=applied(request,0,41,target-2);
    fgs_publication_record(&history,generation);
    const fgs_applied_generation *journal=fgs_publication_find(&history,generation.id);
    CHECK(journal && journal->apply_ns==target-2,"journal keeps actual apply-return timestamp");
    fgs_real_observation real=newer_real(generation,target-1,target-1);
    CHECK(fgs_publication_late_real(journal,real)==FGS_REAL_READY_AFTER_GEN_APPLY,
        "S-1 REAL readiness is potential late-real-after-GEN residual");
    fgs_real_plan plan;
    CHECK(fgs_peek_real(&state,target-1,&plan)==FGS_OK,"later REAL still plans normally");
    CHECK(plan.slot.ordinal>request.slot.ordinal && !plan.generated_conflicts,
        "old occupied-slot conflict metric remains zero after cutoff advanced");
    CHECK(fgs_publication_late_real(journal,real)==FGS_REAL_READY_AFTER_GEN_APPLY,
        "zero occupied-slot conflicts does not erase publication residual");
}
static void equality_and_normal_later_real(void) {
    fgs_state state;
    fgs_request request=prepare_generation(&state);
    uint64_t target=request.slot.target_ns;
    fgs_applied_generation generation=applied(request,0,42,target-100);
    fgs_real_observation real=newer_real(generation,target-100,target-100);
    CHECK(fgs_publication_late_real(&generation,real)==FGS_REAL_READY_AFTER_GEN_APPLY,
        "ready exactly at apply belongs to potential post-apply residual");
    real.ready_ns=target;
    CHECK(fgs_publication_late_real(&generation,real)==FGS_REAL_NOT_LATE,
        "ready at target is not strictly before target");
    real.ready_ns=target+1; real.first_attempt_ns=target+2;
    CHECK(fgs_publication_late_real(&generation,real)==FGS_REAL_NOT_LATE,
        "normal later REAL is not a publication residual");
    real.ready_ns=target-101; real.first_attempt_ns=target-99;
    CHECK(fgs_publication_late_real(&generation,real)==FGS_REAL_READY_BEFORE_GEN_APPLY_UNSERVICED,
        "ready before apply but first serviced after apply has separate classification");
    real.first_attempt_ns=target-100;
    CHECK(fgs_publication_late_real(&generation,real)==FGS_REAL_READY_BEFORE_GEN_APPLY_UNSERVICED,
        "first attempt equality belongs to unserviced classification");
    real.first_attempt_ns=target-101;
    CHECK(fgs_publication_late_real(&generation,real)==FGS_REAL_NOT_LATE,
        "REAL already attempted before GEN apply is not called unserviced");
}
static void unknown_ready_provenance(void) {
    fgs_state state;
    fgs_request request=prepare_generation(&state);
    uint64_t target=request.slot.target_ns;
    fgs_applied_generation generation=applied(request,0,43,target-2);
    fgs_real_observation real=newer_real(generation,0,target-1);
    real.ready_time_known=0;
    CHECK(fgs_publication_late_real(&generation,real)==FGS_REAL_READY_TIME_UNKNOWN,
        "absent backend timestamp is UNKNOWN, not a fabricated later readiness");
    real.ready_ns=target-1;
    CHECK(fgs_publication_late_real(&generation,real)==FGS_REAL_READY_TIME_UNKNOWN,
        "unproven observed-now timestamp cannot imply backend ready-after-apply");
    real.ready_time_known=1; real.ready_ns=0;
    CHECK(fgs_publication_late_real(&generation,real)==FGS_REAL_READY_TIME_UNKNOWN,
        "known flag without timestamp still lacks provenance");
    real.source_epoch++;
    CHECK(fgs_publication_late_real(&generation,real)==FGS_REAL_NOT_LATE,
        "unknown provenance cannot bridge unrelated scene epochs");
    real.source_epoch--; real.window_epoch++;
    CHECK(fgs_publication_late_real(&generation,real)==FGS_REAL_NOT_LATE,
        "unknown provenance cannot bridge window generations");
    real.window_epoch--; real.frame=generation.source_frame;
    CHECK(fgs_publication_late_real(&generation,real)==FGS_REAL_NOT_LATE,
        "GEN source frame is not a newer REAL residual");
}
static void full_capacity_uses_live_tokens(void) {
    fgs_state state;
    fgs_request request=prepare_generation(&state);
    uint64_t target=request.slot.target_ns;
    fgs_publication_history history={0};
    fgs_applied_generation generation=applied(request,0,51,target-2);
    fgs_publication_record(&history,generation);
    fgs_transaction_id live[FGS_PUBLICATION_CONTEXTS]={generation.id,{1,52,7}};
    fgs_capacity_observation wait;
    fgs_publication_capacity_begin(&wait,&history,live,2,target-1);
    CHECK(wait.count==1 && fgs_transaction_equal(wait.generated[0].id,generation.id),
        "full two contexts GEN+REAL attributes exactly the actually live GEN");
    CHECK(fgs_publication_capacity_spans_target(&wait,0,target+1),
        "GEN-associated capacity wait spanning its target is recorded");
    CHECK(!fgs_publication_capacity_spans_target(&wait,0,target-1),
        "GEN association alone does not claim wait spans its target");
    CHECK(!fgs_publication_capacity_spans_target(&wait,1,target+1),
        "nonexistent second GEN cannot create a target span");
    CHECK(!fgs_publication_capacity_spans_target(&wait,0,target-2),
        "reversed wait endpoints cannot create a target span");
    live[0]=(fgs_transaction_id){0,53,7};
    fgs_publication_capacity_begin(&wait,&history,live,2,target-1);
    CHECK(!wait.count,"full two contexts REAL+REAL ignores overlapping retired GEN history");
    CHECK(!fgs_publication_capacity_spans_target(&wait,0,target+1),
        "historical time overlap cannot taint all-REAL capacity wait");
    live[0]=generation.id;
    fgs_publication_capacity_begin(&wait,&history,live,2,target);
    CHECK(fgs_publication_capacity_spans_target(&wait,0,target),
        "capacity target span includes equality at both endpoints");
    fgs_publication_capacity_begin(&wait,&history,live,2,target+1);
    CHECK(wait.count==1 && !fgs_publication_capacity_spans_target(&wait,0,target+2),
        "live GEN is associated even after target without claiming a span");
}
static void late_callback_and_out_of_order_tokens(void) {
    fgs_state state;
    fgs_request request=prepare_generation(&state);
    uint64_t target=request.slot.target_ns;
    fgs_publication_history history={0};
    fgs_applied_generation first=applied(request,0,61,target-2);
    fgs_applied_generation second_gen=applied(request,1,62,target-1);
    second_gen.source_epoch++;
    fgs_publication_record(&history,first);
    fgs_publication_record(&history,second_gen);
    fgs_transaction_id live[FGS_PUBLICATION_CONTEXTS]={second_gen.id,first.id};
    fgs_capacity_observation wait;
    fgs_publication_capacity_begin(&wait,&history,live,2,target-1);
    CHECK(wait.count==2 && fgs_transaction_equal(wait.generated[0].id,second_gen.id) &&
        fgs_transaction_equal(wait.generated[1].id,first.id),
        "snapshot order and different source epochs do not lose live GEN identities");
    live[1]=(fgs_transaction_id){0,63,7};
    fgs_publication_capacity_begin(&wait,&history,live,2,target-1);
    CHECK(wait.count==1 && fgs_transaction_equal(wait.generated[0].id,second_gen.id),
        "late callback for retired first token cannot attach its reused REAL context");
    live[0].generation++;
    fgs_publication_capacity_begin(&wait,&history,live,2,target-1);
    CHECK(!wait.count,"same index and sequence from another generation cannot match history");
    live[0]=second_gen.id; live[0].index=0;
    CHECK(!fgs_publication_find(&history,live[0]),"index is part of immutable transaction identity");
    CHECK(!fgs_publication_find(&history,(fgs_transaction_id){0,0,7}),
        "empty transaction token never matches history");
    fgs_publication_capacity_begin(&wait,&history,NULL,0,target);
    CHECK(!wait.count && wait.start_ns==target,"empty live queue ignores all history");
}
static void history_ring_does_not_alias_recycled_tokens(void) {
    fgs_state state;
    fgs_request request=prepare_generation(&state);
    fgs_publication_history history={0};
    fgs_applied_generation oldest=applied(request,0,1,request.slot.target_ns-2);
    for (unsigned i=1;i<=FGS_PUBLICATION_HISTORY+1;i++) {
        fgs_applied_generation generation=oldest;
        generation.id.index=i%FGS_PUBLICATION_CONTEXTS;
        generation.id.sequence=i;
        fgs_publication_record(&history,generation);
    }
    oldest.id.index=1;
    CHECK(!fgs_publication_find(&history,oldest.id),"evicted history never aliases a recycled context");
    fgs_transaction_id latest={(FGS_PUBLICATION_HISTORY+1)%FGS_PUBLICATION_CONTEXTS,
        FGS_PUBLICATION_HISTORY+1,oldest.id.generation};
    CHECK(fgs_publication_find(&history,latest)!=NULL,"newest actual transaction remains in ring");
}
int main(void) {
    late_real_does_not_redefine_occupied_conflicts();
    equality_and_normal_later_real();
    unknown_ready_provenance();
    full_capacity_uses_live_tokens();
    late_callback_and_out_of_order_tokens();
    history_ring_does_not_alias_recycled_tokens();
    printf("adaptive publication: %u checks passed\n",checks);
    return 0;
}
